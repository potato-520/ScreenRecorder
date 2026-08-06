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

// Helper to convert std::wstring to std::string
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

    m_ffmpegPath = GetFFmpegPath();
    if (m_ffmpegPath.empty()) {
        std::cerr << "FFmpeg not found in application directory!" << std::endl;
        return false;
    }

    // Auto-detect the best hardware encoder
    DetectBestEncoder();
    m_initialized = true;
    return true;
}

std::string RecordingEngine::GetFFmpegPath() const {
    // Check inside %APPDATA%\ScreenRecorder\ffmpeg.exe (our static packaging target)
    wchar_t appDataPath[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath);
    std::wstring ffmpegPath = std::wstring(appDataPath) + L"\\ScreenRecorder\\ffmpeg.exe";

    if (GetFileAttributesW(ffmpegPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return WStringToString(ffmpegPath);
    }

    // Fallback: check system path
    return "ffmpeg.exe";
}

bool RecordingEngine::ProbeEncoder(const std::string& encoderName) {
    // Run a tiny test encoding of 1 frame with the target encoder
    // Command: ffmpeg.exe -y -f lavfi -i color=c=black:s=64x64 -frames:v 1 -c:v ENCODER -f null -
    std::string cmd = "\"" + m_ffmpegPath + "\" -y -f lavfi -i color=c=black:s=64x64 -frames:v 1 -c:v " + encoderName + " -f null -";

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    // Copy command line to mutable buffer
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    BOOL success = CreateProcessA(
        NULL, cmdBuf.data(), NULL, NULL, FALSE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi
    );

    if (!success) {
        return false;
    }

    // Wait for probe process to complete (max 2 seconds)
    DWORD waitResult = WaitForSingleObject(pi.hProcess, 2000);
    DWORD exitCode = 1;
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exitCode);
    } else {
        // Hung probe, terminate it
        TerminateProcess(pi.hProcess, 1);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return (exitCode == 0);
}

void RecordingEngine::DetectBestEncoder() {
    // 1. Query GPU manufacturer using DXGI
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

    // 2. Sort GPU encoders based on detected hardware
    std::vector<std::pair<std::string, std::string>> candidates;
    if (vendorId == 0x10DE) { // NVIDIA
        candidates = {
            {"h264_nvenc", "NVIDIA NVENC (加速)"},
            {"h264_mf", "Windows Media Foundation (GPU)"},
            {"libx264", "CPU (兼容模式)"}
        };
    } else if (vendorId == 0x1002 || vendorId == 0x1022) { // AMD
        candidates = {
            {"h264_amf", "AMD AMF (加速)"},
            {"h264_mf", "Windows Media Foundation (GPU)"},
            {"libx264", "CPU (兼容模式)"}
        };
    } else if (vendorId == 0x8086) { // Intel
        candidates = {
            {"h264_qsv", "Intel QuickSync (加速)"},
            {"h264_mf", "Windows Media Foundation (GPU)"},
            {"libx264", "CPU (兼容模式)"}
        };
    } else { // Generic or unknown
        candidates = {
            {"h264_mf", "Windows Media Foundation (GPU)"},
            {"h264_nvenc", "NVIDIA NVENC (加速)"},
            {"h264_amf", "AMD AMF (加速)"},
            {"h264_qsv", "Intel QuickSync (加速)"},
            {"libx264", "CPU (兼容模式)"}
        };
    }

    // 3. Test candidates using the FFmpeg probe
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

    // Initialize COM (usually already initialized by WebView2, but safe to call)
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
    // eCapture for input/microphone endpoints
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
        outError = "Already recording.";
        return false;
    }

    // 1. Generate filename using local time
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm bt = {};
    localtime_s(&bt, &in_time_t);

    std::ostringstream ss;
    ss << "Record_" 
       << std::put_time(&bt, "%Y%m%d_%H%M%S") 
       << ".mp4";
    outFilename = ss.str();

    // 2. Prepare output path
    std::string outPath = outputFolder;
    if (outPath.empty()) {
        outError = "Output folder is empty.";
        return false;
    }
    
    // Ensure folder has a trailing slash
    char lastChar = outPath.back();
    if (lastChar != '\\' && lastChar != '/') {
        outPath += "\\";
    }
    
    // Create output folder if it doesn't exist
    CreateDirectoryA(outPath.c_str(), NULL);
    std::string fullOutputPath = outPath + outFilename;

    // Ensure width and height are even (h264 requirements)
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

    // 3. Assemble FFmpeg command
    std::ostringstream cmd;
    cmd << "\"" << m_ffmpegPath << "\" -y";

    // Video input (GDIGRAB)
    cmd << " -f gdigrab -framerate 30 -offset_x " << finalX << " -offset_y " << finalY 
        << " -video_size " << finalWidth << "x" << finalHeight << " -i desktop";

    bool hasMicInput = recordMic && !micDeviceName.empty();
    if (hasMicInput) {
        // Use DirectShow (dshow) for microphone capture on Windows
        cmd << " -f dshow -i audio=\"" << micDeviceName << "\"";
    }

    m_recordSysAudio = recordSysAudio;
    m_hasMicInput = hasMicInput;
    m_finalOutputPath = fullOutputPath;
    m_hAudioPipe = INVALID_HANDLE_VALUE;

    if (m_recordSysAudio) {
        // Query system mix format for sample rate and channels
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

        // Create Named Pipe for loopback audio transmission
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

    // Force constant framerate (CFR) to prevent non-monotonic DTS warnings and video timestamp drift
    cmd << " -fps_mode cfr";

    // Encoder configuration (utilize auto-detected best encoder)
    cmd << " -c:v " << m_bestEncoder;
    if (m_bestEncoder == "libx264") {
        // libx264: ultrafast for screen recording, yuv420p for max compatibility
        cmd << " -preset ultrafast -pix_fmt yuv420p";
    } else if (m_bestEncoder == "h264_nvenc") {
        // NVIDIA NVENC: -tune is NOT supported, use -zerolatency for low-latency capture
        cmd << " -preset fast -pix_fmt yuv420p -zerolatency 1";
    } else if (m_bestEncoder == "h264_amf") {
        // AMD AMF: speed quality mode
        cmd << " -pix_fmt yuv420p -quality speed";
    } else if (m_bestEncoder == "h264_qsv") {
        // Intel QSV: nv12 pixel format required
        cmd << " -pix_fmt nv12 -preset veryfast";
    } else { // h264_mf or others
        cmd << " -pix_fmt yuv420p";
    }

    // Audio mixing and output mapping
    if (hasMicInput && m_recordSysAudio) {
        cmd << " -filter_complex \"[1:a][2:a]amix=inputs=2[a]\" -map 0:v -map \"[a]\" -c:a aac -b:a 128k";
    } else if (hasMicInput) {
        cmd << " -map 0:v -map 1:a -c:a aac -b:a 128k";
    } else if (m_recordSysAudio) {
        cmd << " -map 0:v -map 1:a -c:a aac -b:a 128k";
    } else {
        cmd << " -an";
    }

    // Use fragmented MP4 (fMP4) for crash-safe output:
    // - frag_keyframe: write a new fragment at each keyframe
    // - empty_moov:    write an empty moov atom at start so file is immediately playable
    // - default_base_moof: improves player compatibility
    // This ensures the file is playable even if the process crashes mid-recording.
    cmd << " -movflags frag_keyframe+empty_moov+default_base_moof";

    // Output file path directly
    cmd << " \"" << m_finalOutputPath << "\"";


    std::string cmdStr = cmd.str();

    // 4. Set up security attributes for inheritable pipe
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE hPipeRead = nullptr;
    if (!CreatePipe(&hPipeRead, &m_hStdInWrite, &saAttr, 0)) {
        outError = "Failed to create stdin pipe.";
        return false;
    }

    // Ensure the write handle to the pipe is not inherited
    if (!SetHandleInformation(m_hStdInWrite, HANDLE_FLAG_INHERIT, 0)) {
        outError = "Failed to set handle info on pipe write handle.";
        CloseHandle(hPipeRead);
        CloseHandle(m_hStdInWrite);
        m_hStdInWrite = nullptr;
        return false;
    }

    // 5. Start process
    // Create a log file for FFmpeg output in the target folder to prevent blockages/crashes
    std::string logPath = outputFolder;
    if (!logPath.empty()) {
        char lastChar = logPath.back();
        if (lastChar != '\\' && lastChar != '/') {
            logPath += "\\";
        }
    }
    std::string logFile = logPath + "ffmpeg_log.txt";

    // Re-use saAttr which allows handle inheritance
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
    si.wShowWindow = SW_HIDE; // Hide console window completely

    // Copy command to mutable buffer
    std::vector<char> cmdBuf(cmdStr.begin(), cmdStr.end());
    cmdBuf.push_back('\0');

    BOOL success = CreateProcessA(
        NULL, cmdBuf.data(), NULL, NULL, TRUE, // TRUE to inherit handles (crucial for pipe)
        CREATE_NO_WINDOW, NULL, NULL, &si, &m_pi
    );

    // Read handle and log file handle no longer needed in parent process
    CloseHandle(hPipeRead);
    if (hLogFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hLogFile);
    }

    if (!success) {
        outError = "Failed to spawn FFmpeg. Path: " + m_ffmpegPath;
        CloseHandle(m_hStdInWrite);
        m_hStdInWrite = nullptr;
        if (m_hAudioPipe != INVALID_HANDLE_VALUE) {
            CloseHandle(m_hAudioPipe);
            m_hAudioPipe = INVALID_HANDLE_VALUE;
        }
        return false;
    }

    // Start native WASAPI loopback audio capture thread feeding the named pipe
    if (m_recordSysAudio && m_hAudioPipe != INVALID_HANDLE_VALUE) {
        m_loopback.Start(m_hAudioPipe);
    }

    m_recording = true;
    return true;
}

bool RecordingEngine::StopRecording(std::string& outError) {
    if (!m_recording) {
        outError = "Not recording.";
        return false;
    }

    // 1. Stop native audio loopback if active, sending EOF to FFmpeg
    if (m_recordSysAudio) {
        m_loopback.Stop();
    }
    if (m_hAudioPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hAudioPipe);
        m_hAudioPipe = INVALID_HANDLE_VALUE;
    }

    // 2. Write 'q\n' to FFmpeg stdin to stop it gracefully
    if (m_hStdInWrite) {
        DWORD bytesWritten = 0;
        WriteFile(m_hStdInWrite, "q\n", 2, &bytesWritten, NULL);
        CloseHandle(m_hStdInWrite);
        m_hStdInWrite = nullptr;
    }

    // 3. Wait for FFmpeg process to finalize the file (up to 30 seconds for large files)
    DWORD waitResult = WaitForSingleObject(m_pi.hProcess, 30000);
    if (waitResult != WAIT_OBJECT_0) {
        // If it doesn't close gracefully, terminate it
        TerminateProcess(m_pi.hProcess, 1);
        outError = "FFmpeg did not exit gracefully within 30s, terminated.";
    }

    CloseHandle(m_pi.hProcess);
    CloseHandle(m_pi.hThread);
    m_pi = {};

    m_recording = false;
    return true;
}
