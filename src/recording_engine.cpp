#include "recording_engine.h"
#include <shlobj.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <dxgi.h>

/// 辅助工具：将 std::wstring 转换为 UTF-8 编码的 std::string
static std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

RecordingEngine::RecordingEngine() {}

RecordingEngine::~RecordingEngine() {
    std::string err;
    if (m_recording) {
        StopRecording(err);
    }
}

bool RecordingEngine::Init() {
    if (m_initialized) return true;

    // 1. 获取释放于 AppData 目录下的 ffmpeg.exe 路径
    m_ffmpegPath = GetFFmpegPath();
    if (m_ffmpegPath.empty()) {
        std::cerr << "未找到 FFmpeg 录制引擎路径！" << std::endl;
        return false;
    }

    // 2. 自动检测并选择最佳显卡硬件编码器
    DetectBestEncoder();
    m_initialized = true;
    return true;
}

std::string RecordingEngine::GetFFmpegPath() const {
    // 优先检查 AppData\ScreenRecorder\ffmpeg.exe
    wchar_t appDataPath[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath);
    std::wstring ffmpegPath = std::wstring(appDataPath) + L"\\ScreenRecorder\\ffmpeg.exe";

    if (GetFileAttributesW(ffmpegPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return WStringToString(ffmpegPath);
    }

    // 回退机制：若无释放文件则尝试使用环境变量 PATH 中的 ffmpeg.exe
    return "ffmpeg.exe";
}

bool RecordingEngine::ProbeEncoder(const std::string& encoderName) {
    // 运行 1 帧的微型测试命令来验证目标编码器是否在当前显卡驱动下可用
    // 测试指令：ffmpeg.exe -y -f lavfi -i color=c=black:s=64x64 -frames:v 1 -c:v ENCODER -f null -
    std::string cmd = "\"" + m_ffmpegPath + "\" -y -f lavfi -i color=c=black:s=64x64 -frames:v 1 -c:v " + encoderName + " -f null -";

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    BOOL success = CreateProcessA(
        NULL, cmdBuf.data(), NULL, NULL, FALSE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi
    );

    if (!success) {
        return false;
    }

    // 最多等待测试子进程执行 2 秒
    DWORD waitResult = WaitForSingleObject(pi.hProcess, 2000);
    DWORD exitCode = 1;
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exitCode);
    } else {
        // 若测试超时卡死，强制终止子进程
        TerminateProcess(pi.hProcess, 1);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return (exitCode == 0);
}

void RecordingEngine::DetectBestEncoder() {
    // 1. 通过 DXGI API 查询第一块显卡供应商 VendorID
    UINT vendorId = 0;
    IDXGIFactory1* pFactory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&pFactory)))) {
        IDXGIAdapter1* pAdapter = nullptr;
        if (SUCCEEDED(pFactory->EnumAdapters1(0, &pAdapter))) {
            DXGI_ADAPTER_DESC1 desc;
            if (SUCCEEDED(pAdapter->GetDesc1(&desc))) {
                vendorId = desc.VendorId;
            }
            pAdapter->Release();
        }
        pFactory->Release();
    }

    // 2. 根据显卡厂商排列编码器候选优先级
    std::vector<std::pair<std::string, std::string>> candidates;
    if (vendorId == 0x10DE) { // NVIDIA N卡
        candidates = {
            {"h264_nvenc", "NVIDIA NVENC (硬件加速)"},
            {"h264_mf", "Windows Media Foundation (GPU)"},
            {"libx264", "CPU (兼容模式)"}
        };
    } else if (vendorId == 0x1002 || vendorId == 0x1022) { // AMD A卡
        candidates = {
            {"h264_amf", "AMD AMF (硬件加速)"},
            {"h264_mf", "Windows Media Foundation (GPU)"},
            {"libx264", "CPU (兼容模式)"}
        };
    } else if (vendorId == 0x8086) { // Intel I卡/核显
        candidates = {
            {"h264_qsv", "Intel QuickSync (硬件加速)"},
            {"h264_mf", "Windows Media Foundation (GPU)"},
            {"libx264", "CPU (兼容模式)"}
        };
    } else { // 独立显卡未知或通用环境
        candidates = {
            {"h264_mf", "Windows Media Foundation (GPU)"},
            {"h264_nvenc", "NVIDIA NVENC (硬件加速)"},
            {"h264_amf", "AMD AMF (硬件加速)"},
            {"h264_qsv", "Intel QuickSync (硬件加速)"},
            {"libx264", "CPU (兼容模式)"}
        };
    }

    // 3. 顺序探测可用的编码器
    for (const auto& candidate : candidates) {
        if (candidate.first == "libx264") {
            m_bestEncoder = candidate.first;
            m_bestEncoderFriendly = candidate.second;
            break;
        }

        if (ProbeEncoder(candidate.first)) {
            m_bestEncoder = candidate.first;
            m_bestEncoderFriendly = candidate.second;
            break;
        }
    }
}

std::vector<AudioDevice> RecordingEngine::GetMicrophoneDevices() {
    std::vector<AudioDevice> devices;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    IMMDeviceEnumerator* pEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator
    );

    if (FAILED(hr)) {
        CoUninitialize();
        return devices;
    }

    IMMDeviceCollection* pCollection = nullptr;
    // eCapture: 抓取麦克风输入音频终端
    hr = pEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCollection);
    if (SUCCEEDED(hr)) {
        UINT count = 0;
        pCollection->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* pEndpoint = nullptr;
            if (SUCCEEDED(pCollection->Item(i, &pEndpoint))) {
                LPWSTR pstrID = nullptr;
                pEndpoint->GetId(&pstrID);

                IPropertyStore* pProps = nullptr;
                if (SUCCEEDED(pEndpoint->OpenPropertyStore(STGM_READ, &pProps))) {
                    PROPVARIANT varName;
                    PropVariantInit(&varName);
                    if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName))) {
                        AudioDevice dev;
                        dev.id = WStringToString(pstrID);
                        dev.name = WStringToString(varName.pwszVal);
                        devices.push_back(dev);
                        PropVariantClear(&varName);
                    }
                    pProps->Release();
                }
                CoTaskMemFree(pstrID);
                pEndpoint->Release();
            }
        }
        pCollection->Release();
    }
    pEnumerator->Release();
    CoUninitialize();

    return devices;
}

bool RecordingEngine::StartRecording(
    int x, int y, int width, int height,
    bool recordMic, const std::string& micDeviceName,
    bool recordSysAudio,
    const std::string& outputFolder,
    std::string& outFilename,
    std::string& outError
) {
    if (m_recording) {
        outError = "已在录制中。";
        return false;
    }

    // 1. 生成基于当前本地时间的录像文件名
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm bt = {};
    localtime_s(&bt, &in_time_t);

    std::ostringstream ss;
    ss << "Record_" 
       << std::put_time(&bt, "%Y%m%d_%H%M%S") 
       << ".mp4";
    outFilename = ss.str();

    // 2. 准备输出路径与创建目录
    std::string outPath = outputFolder;
    if (outPath.empty()) {
        outError = "输出保存目录不能为空。";
        return false;
    }
    
    char lastChar = outPath.back();
    if (lastChar != '\\' && lastChar != '/') {
        outPath += "\\";
    }
    
    CreateDirectoryA(outPath.c_str(), NULL);
    std::string fullOutputPath = outPath + outFilename;

    // 校正录制区域坐标与偶数尺寸要求 (满足 H.264 4:2:0 采样对齐)
    int finalX = x;
    int finalY = y;
    int finalWidth = width;
    int finalHeight = height;
    if (finalWidth <= 0 || finalHeight <= 0) {
        finalX = 0;
        finalY = 0;
        finalWidth = GetSystemMetrics(SM_CXSCREEN);
        finalHeight = GetSystemMetrics(SM_CYSCREEN);
    }
    if (finalWidth % 2 != 0) finalWidth--;
    if (finalHeight % 2 != 0) finalHeight--;
    if (abs(finalX) % 2 != 0) finalX = (finalX > 0) ? finalX - 1 : finalX + 1;
    if (abs(finalY) % 2 != 0) finalY = (finalY > 0) ? finalY - 1 : finalY + 1;

    // 3. 拼接 FFmpeg 命令行参数
    std::ostringstream cmd;
    cmd << "\"" << m_ffmpegPath << "\" -y";

    // 视频输入流 (使用 Windows GDI 设备抓取桌面)
    cmd << " -f gdigrab -framerate 30 -offset_x " << finalX << " -offset_y " << finalY 
        << " -video_size " << finalWidth << "x" << finalHeight << " -i desktop";

    bool hasMicInput = recordMic && !micDeviceName.empty();
    if (hasMicInput) {
        // 使用 Windows DirectShow (dshow) 抓取麦克风输入
        cmd << " -f dshow -i audio=\"" << micDeviceName << "\"";
    }

    m_recordSysAudio = recordSysAudio;
    m_hasMicInput = hasMicInput;
    m_finalOutputPath = fullOutputPath;
    m_hAudioPipe = INVALID_HANDLE_VALUE;

    if (m_recordSysAudio) {
        // 查询默认声卡的采样率与声道数
        int sampleRate = 48000;
        int channels = 2;

        IMMDeviceEnumerator* pEnumerator = NULL;
        IMMDevice* pDevice = NULL;
        IAudioClient* pAudioClient = NULL;
        WAVEFORMATEX* pwfx = NULL;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator))) {
            if (SUCCEEDED(pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice))) {
                if (SUCCEEDED(pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient))) {
                    if (SUCCEEDED(pAudioClient->GetMixFormat(&pwfx))) {
                        sampleRate = pwfx->nSamplesPerSec;
                        channels = pwfx->nChannels;
                        CoTaskMemFree(pwfx);
                    }
                    pAudioClient->Release();
                }
                pDevice->Release();
            }
            pEnumerator->Release();
        }

        // 创建传输系统回放音频的命名管道 (Named Pipe)
        m_hAudioPipe = CreateNamedPipeA(
            "\\\\.\\pipe\\ScreenRecorderAudioPipe",
            PIPE_ACCESS_OUTBOUND,
            PIPE_TYPE_BYTE | PIPE_WAIT,
            1,
            65536, 65536,
            0, NULL
        );

        if (m_hAudioPipe != INVALID_HANDLE_VALUE) {
            cmd << " -f f32le -ar " << sampleRate << " -ac " << channels 
                << " -i \\\\.\\pipe\\ScreenRecorderAudioPipe";
        }
    }

    // 强制输出恒定帧率 (CFR)，防止帧率漂移与时间戳非单调警告
    cmd << " -fps_mode cfr";

    // 编码器参数配置
    cmd << " -c:v " << m_bestEncoder;
    if (m_bestEncoder == "libx264") {
        cmd << " -preset ultrafast -pix_fmt yuv420p";
    } else if (m_bestEncoder == "h264_nvenc") {
        cmd << " -preset fast -pix_fmt yuv420p -zerolatency 1";
    } else if (m_bestEncoder == "h264_amf") {
        cmd << " -pix_fmt yuv420p -quality speed";
    } else if (m_bestEncoder == "h264_qsv") {
        cmd << " -pix_fmt nv12 -preset veryfast";
    } else {
        cmd << " -pix_fmt yuv420p";
    }

    // 音频混音与映射配置
    if (hasMicInput && m_recordSysAudio) {
        cmd << " -filter_complex \"[1:a][2:a]amix=inputs=2[a]\" -map 0:v -map \"[a]\" -c:a aac -b:a 128k";
    } else if (hasMicInput) {
        cmd << " -map 0:v -map 1:a -c:a aac -b:a 128k";
    } else if (m_recordSysAudio) {
        cmd << " -map 0:v -map 1:a -c:a aac -b:a 128k";
    } else {
        cmd << " -an";
    }

    // 使用分片 MP4 (fMP4) 提高防崩溃容错性
    cmd << " -movflags frag_keyframe+empty_moov+default_base_moof";
    cmd << " \"" << m_finalOutputPath << "\"";

    std::string cmdStr = cmd.str();

    // 4. 创建继承的匿名管道用于传输 'q' 键停止信号给 FFmpeg stdin
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE hPipeRead = nullptr;
    if (!CreatePipe(&hPipeRead, &m_hStdInWrite, &saAttr, 0)) {
        outError = "创建 stdin 管道失败。";
        return false;
    }

    SetHandleInformation(m_hStdInWrite, HANDLE_FLAG_INHERIT, 0);

    // 5. 启动 FFmpeg 录制子进程
    std::string logPath = outputFolder;
    if (!logPath.empty()) {
        char lastChar = logPath.back();
        if (lastChar != '\\' && lastChar != '/') {
            logPath += "\\";
        }
    }
    std::string logFile = logPath + "ffmpeg_log.txt";

    HANDLE hLogFile = CreateFileA(
        logFile.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &saAttr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    STARTUPINFOA si = { sizeof(si) };
    si.hStdInput = hPipeRead;
    if (hLogFile != INVALID_HANDLE_VALUE) {
        si.hStdOutput = hLogFile;
        si.hStdError = hLogFile;
    } else {
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // 静默后台运行控制台

    std::vector<char> cmdBuf(cmdStr.begin(), cmdStr.end());
    cmdBuf.push_back('\0');

    BOOL success = CreateProcessA(
        NULL, cmdBuf.data(), NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &m_pi
    );

    CloseHandle(hPipeRead);
    if (hLogFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hLogFile);
    }

    if (!success) {
        outError = "启动 FFmpeg 失败，路径: " + m_ffmpegPath;
        CloseHandle(m_hStdInWrite);
        m_hStdInWrite = nullptr;
        if (m_hAudioPipe != INVALID_HANDLE_VALUE) {
            CloseHandle(m_hAudioPipe);
            m_hAudioPipe = INVALID_HANDLE_VALUE;
        }
        return false;
    }

    // 启动原生 WASAPI 音频采集线程发送数据至命名管道
    if (m_recordSysAudio && m_hAudioPipe != INVALID_HANDLE_VALUE) {
        m_loopback.Start(m_hAudioPipe);
    }

    m_recording = true;
    return true;
}

bool RecordingEngine::StopRecording(std::string& outError) {
    if (!m_recording) {
        outError = "未在录制。";
        return false;
    }

    // 1. 停止 WASAPI 音频采集并关闭管道
    if (m_recordSysAudio) {
        m_loopback.Stop();
    }
    if (m_hAudioPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hAudioPipe);
        m_hAudioPipe = INVALID_HANDLE_VALUE;
    }

    // 2. 向 FFmpeg 标准输入 (stdin) 发送 'q\n' 指令优雅结束 MP4 文件写入
    if (m_hStdInWrite) {
        DWORD bytesWritten = 0;
        WriteFile(m_hStdInWrite, "q\n", 2, &bytesWritten, NULL);
        CloseHandle(m_hStdInWrite);
        m_hStdInWrite = nullptr;
    }

    // 3. 等待 FFmpeg 进程退出并释放句柄 (最多等待 30 秒)
    DWORD waitResult = WaitForSingleObject(m_pi.hProcess, 30000);
    if (waitResult != WAIT_OBJECT_0) {
        TerminateProcess(m_pi.hProcess, 1);
        outError = "FFmpeg 未能在 30 秒内退出，已被强制终止。";
    }

    CloseHandle(m_pi.hProcess);
    CloseHandle(m_pi.hThread);
    m_pi = {};

    m_recording = false;
    return true;
}
