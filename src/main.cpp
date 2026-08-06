#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <wrl.h>
#include "WebView2.h"

#include "recording_engine.h"
#include "selection_overlay.h"
#include "border_window.h"
#include "resource.h"

using namespace Microsoft::WRL;

// ==========================================
// 全局变量定义
// ==========================================
HWND g_hWnd = NULL;                          // 主窗口句柄
HINSTANCE g_hInstance = NULL;                // 应用程序实例句柄
ICoreWebView2Controller* g_webController = nullptr; // WebView2 控制器接口
ICoreWebView2* g_webView = nullptr;          // WebView2 核心接口
RecordingEngine g_recorder;                  // FFmpeg 录制引擎单例
RecordBorder g_border;                       // 交互式录制边框窗口单例
bool g_isCompact = false;                    // 当前是否处于紧凑/悬浮条模式
RECT g_normalRect = { 0, 0, 620, 580 };      // 保存普通模式下的窗口位置尺寸
int g_cropX = 0, g_cropY = 0, g_cropW = 0, g_cropH = 0; // 当前选中的录像坐标与分辨率

// 资源与显卡初始化完成自定义消息
#define WM_USER_INIT_DONE (WM_USER + 100)
static bool g_isInitComplete = false;
static std::string g_initStatusJson = "";

// 函数前向声明
void SendWebMessage(const std::string& message);

// ==========================================
// 系统托盘图标管理与菜单
// ==========================================
#define WM_TRAYNOTIFY       (WM_USER + 1)
#define IDM_TRAY_SHOW       2001
#define IDM_TRAY_START_STOP 2002
#define IDM_TRAY_QUIT       2003

NOTIFYICONDATA g_trayNid = {};
bool g_trayAdded = false;

/// 添加系统托盘图标
static void AddTrayIcon(HWND hWnd, HINSTANCE hInst) {
    if (g_trayAdded) return;
    ZeroMemory(&g_trayNid, sizeof(g_trayNid));
    g_trayNid.cbSize           = sizeof(NOTIFYICONDATA);
    g_trayNid.hWnd             = hWnd;
    g_trayNid.uID              = 1;
    g_trayNid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayNid.uCallbackMessage = WM_TRAYNOTIFY;
    g_trayNid.hIcon            = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    wcscpy_s(g_trayNid.szTip, L"智眸录屏");
    Shell_NotifyIconW(NIM_ADD, &g_trayNid);
    g_trayAdded = true;
}

/// 移除系统托盘图标
static void RemoveTrayIcon() {
    if (!g_trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_trayNid);
    g_trayAdded = false;
}

/// 弹出托盘右键菜单
static void ShowTrayMenu(HWND hWnd) {
    HMENU hMenu = CreatePopupMenu();
    bool isRecording = g_recorder.IsRecording();

    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW, L"显示主界面");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_START_STOP,
        isRecording ? L"⏹  停止录制" : L"⏺  开始录制");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_QUIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);
    UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                              pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);

    if (cmd == IDM_TRAY_SHOW) {
        ShowWindow(hWnd, SW_RESTORE);
        SetForegroundWindow(hWnd);
    } else if (cmd == IDM_TRAY_START_STOP) {
        // 转发开始/停止指令给前端 JS
        if (g_webView) {
            if (isRecording) {
                g_webView->ExecuteScript(L"window.__stopRecording && window.__stopRecording();", nullptr);
            } else {
                g_webView->ExecuteScript(L"window.__startRecording && window.__startRecording();", nullptr);
            }
        }
    } else if (cmd == IDM_TRAY_QUIT) {
        RemoveTrayIcon();
        PostQuitMessage(0);
    }
}

/// 调试日志输出工具
static void LogDebug(const std::string& text) {
    wchar_t myVideosPath[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_MYVIDEO, NULL, 0, myVideosPath);
    std::wstring logPath = std::wstring(myVideosPath) + L"\\resize_debug_log.txt";

    FILE* f = NULL;
    _wfopen_s(&f, logPath.c_str(), L"a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, text.c_str());
        fclose(f);
    }
}

// ==========================================
// 文件哈希校验与嵌入资源释放工具
// ==========================================
static std::string GetFileMD5(const std::wstring& filePath) {
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return "";

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::string md5Result = "";

    if (CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(hProv, CALG_MD5, 0, 0, &hHash)) {
            BYTE buffer[1024 * 64];
            DWORD bytesRead = 0;
            bool success = true;
            while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0) {
                if (!CryptHashData(hHash, buffer, bytesRead, 0)) {
                    success = false;
                    break;
                }
            }
            if (success) {
                BYTE hashVal[16];
                DWORD hashLen = 16;
                if (CryptGetHashParam(hHash, HP_HASHVAL, hashVal, &hashLen, 0)) {
                    char hex[33];
                    for (int i = 0; i < 16; i++) {
                        sprintf_s(hex + i * 2, 3, "%02x", hashVal[i]);
                    }
                    md5Result = hex;
                }
            }
            CryptDestroyHash(hHash);
        }
        CryptReleaseContext(hProv, 0);
    }
    CloseHandle(hFile);
    return md5Result;
}

static std::string GetBufferMD5(const BYTE* buffer, DWORD size) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::string md5Result = "";

    if (CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(hProv, CALG_MD5, 0, 0, &hHash)) {
            if (CryptHashData(hHash, buffer, size, 0)) {
                BYTE hashVal[16];
                DWORD hashLen = 16;
                if (CryptGetHashParam(hHash, HP_HASHVAL, hashVal, &hashLen, 0)) {
                    char hex[33];
                    for (int i = 0; i < 16; i++) {
                        sprintf_s(hex + i * 2, 3, "%02x", hashVal[i]);
                    }
                    md5Result = hex;
                }
            }
            CryptDestroyHash(hHash);
        }
        CryptReleaseContext(hProv, 0);
    }
    return md5Result;
}

/// 释放 exe 内置静态资源至本地 AppData 目录
static bool ExtractResource(int resourceId, const std::wstring& targetPath, bool forceOverwrite = false, bool isFfmpeg = false) {
    HRSRC hRes = FindResourceW(g_hInstance, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!hRes) return false;

    HGLOBAL hData = LoadResource(g_hInstance, hRes);
    if (!hData) return false;

    DWORD size = SizeofResource(g_hInstance, hRes);
    const BYTE* pData = (const BYTE*)LockResource(hData);
    if (!pData) return false;

    // 若非强制覆盖，先进行文件大小与 MD5 哈希快速对比校验，相同则跳过写入
    if (!forceOverwrite) {
        HANDLE hCheck = CreateFileW(targetPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hCheck != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER fileSize;
            if (GetFileSizeEx(hCheck, &fileSize) && fileSize.QuadPart == size) {
                CloseHandle(hCheck);
                std::string fileMd5 = GetFileMD5(targetPath);
                std::string resMd5 = GetBufferMD5(pData, size);
                if (fileMd5 == resMd5) {
                    return true;
                }
            } else {
                CloseHandle(hCheck);
            }
        }
    }

    // 分块写入大文件（如 ffmpeg.exe）
    HANDLE hFile = CreateFileW(targetPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    const DWORD CHUNK_SIZE = 1024 * 1024 * 4; // 4MB 块
    DWORD bytesWrittenTotal = 0;
    
    while (bytesWrittenTotal < size) {
        DWORD chunkToWrite = (size - bytesWrittenTotal > CHUNK_SIZE) ? CHUNK_SIZE : (size - bytesWrittenTotal);
        DWORD bytesWritten = 0;
        if (!WriteFile(hFile, pData + bytesWrittenTotal, chunkToWrite, &bytesWritten, NULL) || bytesWritten != chunkToWrite) {
            CloseHandle(hFile);
            return false;
        }
        bytesWrittenTotal += bytesWritten;
    }

    CloseHandle(hFile);
    return true;
}

static std::wstring GetAppDataRootDir() {
    wchar_t appDataPath[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath);
    std::wstring rootDir = std::wstring(appDataPath) + L"\\ScreenRecorder";
    return rootDir;
}

static bool ExtractAllResources() {
    std::wstring rootDir = GetAppDataRootDir();
    CreateDirectoryW(rootDir.c_str(), NULL);
    CreateDirectoryW((rootDir + L"\\ui").c_str(), NULL);

    bool ok = true;
    ok &= ExtractResource(IDR_FFMPEG_EXE, rootDir + L"\\ffmpeg.exe", false, true);
    ok &= ExtractResource(IDR_HTML, rootDir + L"\\ui\\index.html", true, false);
    ok &= ExtractResource(IDR_CSS, rootDir + L"\\ui\\style.css", true, false);
    ok &= ExtractResource(IDR_JS, rootDir + L"\\ui\\main.js", true, false);
    return ok;
}

// ==========================================
// 界面模式切换 (普通完整模式 <-> 录制紧凑悬浮条)
// ==========================================
void SwitchToCompactMode(int x, int y, int w, int h) {
    if (g_isCompact) return;

    // 记录当前普通模式的窗口几何参数
    GetWindowRect(g_hWnd, &g_normalRect);
    g_isCompact = true;

    UINT dpi = GetDpiForWindow(g_hWnd);
    if (dpi == 0) dpi = 96;

    // 紧凑悬浮控制条尺寸：380w x 80h（结合 DPI 缩放）
    int tbW = MulDiv(380, dpi, 96);
    int tbH = MulDiv(80, dpi, 96);
    int tbX = x + (w - tbW) / 2;
    int tbY = y + h + 12;

    int vsLeft  = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vsTop   = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsRight = vsLeft + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vsBot   = vsTop  + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (tbX < vsLeft) tbX = vsLeft;
    if (tbX + tbW > vsRight) tbX = vsRight - tbW;
    if (tbY + tbH > vsBot) {
        tbY = y - tbH - 12;
        if (tbY < vsTop) tbY = y + 10;
    }

    // 设置主窗口置顶并改变尺寸为悬浮条
    SetWindowPos(g_hWnd, HWND_TOPMOST, tbX, tbY, tbW, tbH, SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    // 弹出捕获区域全穿透边框窗口
    g_border.Create(g_hInstance, x, y, w, h);
    g_border.SetOnAreaChanged([](int newX, int newY, int newW, int newH) {
        g_cropX = newX;
        g_cropY = newY;
        g_cropW = newW;
        g_cropH = newH;

        std::ostringstream ss;
        ss << "{\"action\":\"crop_updated\",\"x\":" << newX << ",\"y\":" << newY 
           << ",\"w\":" << newW << ",\"h\":" << newH << "}";
        SendWebMessage(ss.str());
    });
}

void SwitchToNormalMode() {
    if (!g_isCompact) return;

    // 销毁录制边框
    g_border.Destroy();
    g_isCompact = false;

    // 还原窗口为普通布局与非置顶状态
    int savedW = g_normalRect.right  - g_normalRect.left;
    int savedH = g_normalRect.bottom - g_normalRect.top;

    int vsLeft  = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vsTop   = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsRight = vsLeft + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vsBot   = vsTop  + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    int restoreX = g_normalRect.left;
    int restoreY = g_normalRect.top;
    if (restoreX < vsLeft) restoreX = vsLeft;
    if (restoreY < vsTop)  restoreY = vsTop;
    if (restoreX + savedW > vsRight) restoreX = vsRight - savedW;
    if (restoreY + savedH > vsBot)   restoreY = vsBot   - savedH;

    SetWindowPos(g_hWnd, HWND_NOTOPMOST, restoreX, restoreY, savedW, savedH, SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    // 通知前端 JS 重新计算 DOM 容器高度并请求调整
    if (g_webView) {
        g_webView->ExecuteScript(
            L"(function(){"
            L"  var el = document.getElementById('content-measure-wrapper');"
            L"  if(el){ var h = Math.ceil(el.scrollHeight) + 28;"
            L"    if(window.chrome && window.chrome.webview)"
            L"      window.chrome.webview.postMessage(JSON.stringify({action:'resize_window',h:h}));"
            L"  }"
            L"})()",
            nullptr);
    }
}

// ==========================================
// 字符串编码转换与显示器枚举工具
// ==========================================
static std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

static std::wstring StringToWString(const std::string& str) {
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

struct MonitorDevice {
    std::string name;
    int x, y, w, h;
};

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    auto* list = (std::vector<MonitorDevice>*)dwData;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMonitor, &mi)) {
        MonitorDevice dev;
        dev.x = mi.rcMonitor.left;
        dev.y = mi.rcMonitor.top;
        dev.w = mi.rcMonitor.right - mi.rcMonitor.left;
        dev.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        
        char szFriendly[128];
        sprintf_s(szFriendly, "显示器 %d (%dx%d)", (int)list->size() + 1, dev.w, dev.h);
        dev.name = szFriendly;
        
        list->push_back(dev);
    }
    return TRUE;
}

static std::vector<MonitorDevice> GetMonitorDevices() {
    std::vector<MonitorDevice> list;
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, (LPARAM)&list);
    return list;
}

static std::string EscapeJsonString(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    return out;
}

// ==========================================
// 简单轻量级 JSON 字段提取辅助函数
// ==========================================
static std::string GetJsonStringValue(const std::string& json, const std::string& key) {
    size_t pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = json.find(":", pos);
    if (pos == std::string::npos) return "";
    size_t startQuote = json.find("\"", pos);
    if (startQuote == std::string::npos) return "";
    size_t endQuote = json.find("\"", startQuote + 1);
    if (endQuote == std::string::npos) return "";
    return json.substr(startQuote + 1, endQuote - startQuote - 1);
}

static bool GetJsonBoolValue(const std::string& json, const std::string& key) {
    size_t pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;
    pos = json.find(":", pos);
    if (pos == std::string::npos) return false;
    size_t t = json.find("true", pos);
    size_t f = json.find("false", pos);
    if (t != std::string::npos && (f == std::string::npos || t < f)) {
        return true;
    }
    return false;
}

static int GetJsonIntValue(const std::string& json, const std::string& key) {
    size_t pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return 0;
    pos = json.find(":", pos);
    if (pos == std::string::npos) return 0;
    while (pos < json.size() && !isdigit(json[pos]) && json[pos] != '-') {
        pos++;
    }
    if (pos >= json.size()) return 0;
    return std::stoi(json.substr(pos));
}

/// 获取当前系统的“视频”默认保存路径
std::string GetVideosPath() {
    wchar_t* pPath = NULL;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_DEFAULT, NULL, &pPath);
    if (SUCCEEDED(hr)) {
        std::wstring wPath(pPath);
        CoTaskMemFree(pPath);
        return WStringToString(wPath);
    }
    return "C:\\";
}

/// 发送消息至前端 WebView2 页面
void SendWebMessage(const std::string& message) {
    if (g_webView) {
        std::wstring wMsg = StringToWString(message);
        g_webView->PostWebMessageAsString(wMsg.c_str());
    }
}

// ==========================================
// 核心 IPC 消息分发处理器（处理 WebView2 JS 指令）
// ==========================================
void HandleWebMessage(const std::string& message) {
    std::string action = GetJsonStringValue(message, "action");

    if (action == "get_init_status") {
        if (g_isInitComplete && !g_initStatusJson.empty()) {
            SendWebMessage(g_initStatusJson);
        }
    }
    else if (action == "init") {
        // 枚举麦克风与显示器设备
        auto mics = g_recorder.GetMicrophoneDevices();
        auto monitors = GetMonitorDevices();
        std::string encoder = g_recorder.GetBestEncoderFriendlyName();
        std::string defaultFolder = GetVideosPath();

        std::ostringstream ss;
        ss << "{\"action\":\"init_response\",\"encoder\":\"" << EscapeJsonString(encoder)
           << "\",\"defaultFolder\":\"" << EscapeJsonString(defaultFolder) << "\",\"mics\":[";
        
        for (size_t i = 0; i < mics.size(); i++) {
            ss << "{\"id\":\"" << EscapeJsonString(mics[i].id) 
               << "\",\"name\":\"" << EscapeJsonString(mics[i].name) << "\"}";
            if (i + 1 < mics.size()) ss << ",";
        }
        ss << "],\"monitors\":[";
        for (size_t i = 0; i < monitors.size(); i++) {
            ss << "{\"name\":\"" << EscapeJsonString(monitors[i].name)
               << "\",\"x\":" << monitors[i].x << ",\"y\":" << monitors[i].y
               << ",\"w\":" << monitors[i].w << ",\"h\":" << monitors[i].h << "}";
            if (i + 1 < monitors.size()) ss << ",";
        }
        ss << "]}";
        
        SendWebMessage(ss.str());
    }
    else if (action == "select_area") {
        // 点击“框选区域”：先最小化主窗口，弹出全屏画框蒙版
        ShowWindow(g_hWnd, SW_MINIMIZE);
        
        CropRect rect = SelectionOverlay::Show(g_hInstance);
        
        ShowWindow(g_hWnd, SW_RESTORE);
        SetForegroundWindow(g_hWnd);

        std::ostringstream ss;
        if (rect.cancelled) {
            ss << "{\"action\":\"select_area_response\",\"cancelled\":true}";
        } else {
            g_cropX = rect.x;
            g_cropY = rect.y;
            g_cropW = rect.w;
            g_cropH = rect.h;

            ss << "{\"action\":\"select_area_response\",\"cancelled\":false"
               << ",\"x\":" << rect.x << ",\"y\":" << rect.y 
               << ",\"w\":" << rect.w << ",\"h\":" << rect.h << "}";
            
            // 切换为紧凑悬浮控制条，并弹出交互式录制边框
            SwitchToCompactMode(rect.x, rect.y, rect.w, rect.h);
        }
        SendWebMessage(ss.str());
    }
    else if (action == "start_recording") {
        int x = GetJsonIntValue(message, "x");
        int y = GetJsonIntValue(message, "y");
        int w = GetJsonIntValue(message, "w");
        int h = GetJsonIntValue(message, "h");

        if (g_border.IsActive()) {
            g_border.GetArea(x, y, w, h);
        }

        bool recordMic = GetJsonBoolValue(message, "recordMic");
        std::string micDevice = GetJsonStringValue(message, "micDevice");
        bool recordSysAudio = GetJsonBoolValue(message, "recordSysAudio");
        std::string outputFolder = GetJsonStringValue(message, "outputFolder");

        std::string filename;
        std::string error;
        bool success = g_recorder.StartRecording(
            x, y, w, h, recordMic, micDevice, recordSysAudio, outputFolder, filename, error
        );

        std::ostringstream ss;
        if (success) {
            g_border.SetRecording(true); // 边框变为亮红色

            ss << "{\"action\":\"start_recording_response\",\"success\":true,\"filename\":\"" 
               << EscapeJsonString(filename) << "\"}";
        } else {
            ss << "{\"action\":\"start_recording_response\",\"success\":false,\"error\":\"" 
               << EscapeJsonString(error) << "\"}";
        }
        SendWebMessage(ss.str());
    }
    else if (action == "stop_recording") {
        std::string error;
        bool success = g_recorder.StopRecording(error);

        std::ostringstream ss;
        if (success) {
            g_border.SetRecording(false);
            SwitchToNormalMode(); // 恢复为正常主界面

            ss << "{\"action\":\"stop_recording_response\",\"success\":true}";
        } else {
            ss << "{\"action\":\"stop_recording_response\",\"success\":false,\"error\":\"" 
               << EscapeJsonString(error) << "\"}";
        }
        SendWebMessage(ss.str());
    }
    else if (action == "start_drag") {
        // 无边框自定义标题栏拖拽实现
        ReleaseCapture();
        SendMessage(g_hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
    else if (action == "cancel_selection") {
        SwitchToNormalMode();
    }
    else if (action == "minimize") {
        // 隐藏至系统右下角托盘
        ShowWindow(g_hWnd, SW_HIDE);
        AddTrayIcon(g_hWnd, g_hInstance);
    }
    else if (action == "close") {
        PostQuitMessage(0);
    }
    else if (action == "open_folder") {
        std::string folder = GetJsonStringValue(message, "folder");
        std::wstring wFolder = StringToWString(folder);
        ShellExecute(NULL, L"open", wFolder.c_str(), NULL, NULL, SW_SHOWDEFAULT);
    }
    else if (action == "open_file") {
        std::string file = GetJsonStringValue(message, "file");
        std::wstring wFile = StringToWString(file);
        ShellExecute(NULL, L"open", wFile.c_str(), NULL, NULL, SW_SHOWDEFAULT);
    }
    else if (action == "resize_window") {
        int h = GetJsonIntValue(message, "h");
        if (!g_isCompact && h > 0) {
            UINT dpi = GetDpiForWindow(g_hWnd);
            if (dpi == 0) dpi = 96;

            int physicalWidth = MulDiv(620, dpi, 96);
            int physicalHeight = MulDiv(h, dpi, 96) + 4;

            SetWindowPos(g_hWnd, NULL, 0, 0, physicalWidth, physicalHeight, 
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }
    else if (action == "log") {
        std::string logMsg = GetJsonStringValue(message, "message");
        LogDebug("JS_LOG: " + logMsg);
    }
}

// ==========================================
// Win32 主窗口过程 (WndProc)
// ==========================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCCALCSIZE: {
        if (wParam == TRUE) {
            // 返回 0 去除默认 Win32 标题栏与非客户区外框，实现无边框现代 UI
            return 0;
        }
        break;
    }
    case WM_DPICHANGED: {
        // 多显示器 DPI 动态切换缩放自适应
        RECT* const prcNewWindow = (RECT*)lParam;
        SetWindowPos(hWnd,
            NULL,
            prcNewWindow->left,
            prcNewWindow->top,
            prcNewWindow->right - prcNewWindow->left,
            prcNewWindow->bottom - prcNewWindow->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        break;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* pInfo = (MINMAXINFO*)lParam;
        UINT dpi = GetDpiForWindow(hWnd);
        if (dpi == 0) dpi = 96;

        if (g_isCompact) {
            int tbW = MulDiv(380, dpi, 96);
            int tbH = MulDiv(80, dpi, 96);
            pInfo->ptMinTrackSize.x = tbW;
            pInfo->ptMinTrackSize.y = tbH;
            pInfo->ptMaxTrackSize.x = tbW;
            pInfo->ptMaxTrackSize.y = tbH;
        } else {
            pInfo->ptMinTrackSize.x = MulDiv(500, dpi, 96);
            pInfo->ptMinTrackSize.y = MulDiv(400, dpi, 96);
        }
        return 0;
    }
    case WM_SIZE: {
        if (g_webController != nullptr) {
            RECT bounds;
            GetClientRect(hWnd, &bounds);
            g_webController->put_Bounds(bounds);
        }
        break;
    }
    case WM_SYSCOMMAND: {
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            ShowWindow(hWnd, SW_HIDE);
            AddTrayIcon(hWnd, g_hInstance);
            return 0;
        }
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    case WM_USER_INIT_DONE: {
        SetWindowTextW(hWnd, L"智眸录屏 - 资源加载完成");
        g_isInitComplete = true;

        std::string encoderName = g_recorder.GetBestEncoderFriendlyName();
        std::ostringstream ss;
        ss << "{\"action\":\"init_status\",\"status\":\"ready\",\"encoderFriendly\":\"" 
           << EscapeJsonString(encoderName) << "\"}";
        g_initStatusJson = ss.str();

        if (g_webView) {
            SendWebMessage(g_initStatusJson);
        }
        break;
    }
    case WM_TRAYNOTIFY: {
        switch (lParam) {
        case WM_LBUTTONDBLCLK:
        case WM_LBUTTONUP:
            ShowWindow(hWnd, SW_RESTORE);
            SetForegroundWindow(hWnd);
            break;
        case WM_RBUTTONUP:
            ShowTrayMenu(hWnd);
            break;
        }
        return 0;
    }
    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// ==========================================
// Win32 程序入口点 (wWinMain)
// ==========================================
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow) {
    // 启用 Per-Monitor DPI Awareness (V2) 高分屏自适应
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_hInstance = hInstance;

    // 清理旧日志
    wchar_t myVideosPath[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_MYVIDEO, NULL, 0, myVideosPath);
    std::wstring logPath = std::wstring(myVideosPath) + L"\\resize_debug_log.txt";
    DeleteFileW(logPath.c_str());

    LogDebug("=== ScreenRecorder Session Started ===");

    // 初始化 UI 线程的 COM 组件
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 1;

    // 注册主窗口类
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEX) };
    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.hInstance      = hInstance;
    wcex.hCursor        = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground  = CreateSolidBrush(RGB(12, 14, 18)); // 暗色底色防止闪白
    wcex.lpszClassName  = L"ScreenRecorderMainClass";
    wcex.hIcon          = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wcex.hIconSm        = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    RegisterClassExW(&wcex);

    UINT dpi = GetDpiForSystem();
    if (dpi == 0) dpi = 96;

    int winWidth = MulDiv(620, dpi, 96);
    int winHeight = MulDiv(580, dpi, 96);
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenWidth - winWidth) / 2;
    int posY = (screenHeight - winHeight) / 2;

    // 1. 立即创建并展示主窗口（极速秒开感）
    g_hWnd = CreateWindowW(
        L"ScreenRecorderMainClass", L"智眸录屏 - 资源加载中...", 
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX,
        posX, posY, winWidth, winHeight, 
        NULL, NULL, hInstance, NULL
    );

    if (!g_hWnd) {
        CoUninitialize();
        return 1;
    }

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    // 2. 创建后台工作线程异步执行资源释放与 GPU 编码器探测
    CreateThread(NULL, 0, [](LPVOID) -> DWORD {
        ExtractAllResources();
        g_recorder.Init();
        PostMessage(g_hWnd, WM_USER_INIT_DONE, 0, 0);
        return 0;
    }, NULL, 0, NULL);

    // 3. 初始化 WebView2 渲染环境
    std::wstring userDataFolder = GetAppDataRootDir() + L"\\webview_cache";

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result)) {
                    MessageBox(g_hWnd, L"创建 WebView2 运行环境失败！请确认您的 Windows 已安装 Microsoft Edge Runtime。", L"环境错误", MB_OK | MB_ICONERROR);
                    return result;
                }

                env->CreateCoreWebView2Controller(g_hWnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result)) {
                                MessageBox(g_hWnd, L"创建 WebView2 控制器失败！", L"环境错误", MB_OK | MB_ICONERROR);
                                return result;
                            }

                            g_webController = controller;
                            g_webController->AddRef();

                            RECT bounds;
                            GetClientRect(g_hWnd, &bounds);
                            g_webController->put_Bounds(bounds);

                            g_webController->get_CoreWebView2(&g_webView);
                            g_webView->AddRef();

                            // 禁用内置 F12 右键菜单与缩放，保持极客 UI
                            ICoreWebView2Settings* settings = nullptr;
                            g_webView->get_Settings(&settings);
                            if (settings != nullptr) {
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                            }

                            // 注册前端 postMessage 消息接收回调
                            g_webView->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR messageW = nullptr;
                                        args->TryGetWebMessageAsString(&messageW);
                                        if (messageW != nullptr) {
                                            std::string message = WStringToString(messageW);
                                            CoTaskMemFree(messageW);
                                            HandleWebMessage(message);
                                        }
                                        return S_OK;
                                    }
                                ).Get(), nullptr
                            );

                            // 加载前端 index.html
                            std::wstring rootDir = GetAppDataRootDir();
                            std::wstring htmlPath = L"file:///" + rootDir + L"/ui/index.html";
                            for (size_t i = 0; i < htmlPath.length(); ++i) {
                                if (htmlPath[i] == L'\\') {
                                    htmlPath[i] = L'/';
                                }
                            }

                            g_webView->Navigate(htmlPath.c_str());
                            return S_OK;
                        }
                    ).Get()
                );
                return S_OK;
            }
        ).Get()
    );

    // 主 Win32 消息循环
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 释放 COM 与 WebView2 资源
    if (g_webView) g_webView->Release();
    if (g_webController) g_webController->Release();

    CoUninitialize();
    return (int)msg.wParam;
}
