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

// Global variables
HWND g_hWnd = NULL;
HINSTANCE g_hInstance = NULL;
ICoreWebView2Controller* g_webController = nullptr;
ICoreWebView2* g_webView = nullptr;
RecordingEngine g_recorder;
RecordBorder g_border;
bool g_isCompact = false;
RECT g_normalRect = { 0, 0, 620, 580 };
int g_cropX = 0, g_cropY = 0, g_cropW = 0, g_cropH = 0;

#define WM_USER_INIT_DONE (WM_USER + 100)
static bool g_isInitComplete = false;
static std::string g_initStatusJson = "";

void SendWebMessage(const std::string& message);

// Tray icon constants
#define WM_TRAYNOTIFY       (WM_USER + 1)
#define IDM_TRAY_SHOW       2001
#define IDM_TRAY_START_STOP 2002
#define IDM_TRAY_QUIT       2003

NOTIFYICONDATA g_trayNid = {};
bool g_trayAdded = false;

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

static void RemoveTrayIcon() {
    if (!g_trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_trayNid);
    g_trayAdded = false;
}

static void ShowTrayMenu(HWND hWnd) {
    HMENU hMenu = CreatePopupMenu();
    bool isRecording = g_recorder.IsRecording();

    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW, L"显示主界面");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_START_STOP,
        isRecording ? L"⏹  停止录制" : L"⏺  开始录制");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_QUIT, L"退出");

    // Position menu at cursor
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd); // Required for menu to dismiss properly
    UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                              pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);

    if (cmd == IDM_TRAY_SHOW) {
        ShowWindow(hWnd, SW_RESTORE);
        SetForegroundWindow(hWnd);
    } else if (cmd == IDM_TRAY_START_STOP) {
        // Forward start/stop to WebView JS
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

// Splash Window globals
HWND g_hSplashWnd = NULL;
int g_splashProgress = 0;
std::wstring g_splashStatusText = L"正在启动...";

LRESULT CALLBACK SplashWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        // Double buffering to prevent flicker
        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbmMem = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ hOldObj = SelectObject(hdcMem, hbmMem);

        // Fill background (#0c0e12)
        HBRUSH hBgBrush = CreateSolidBrush(RGB(12, 14, 18));
        FillRect(hdcMem, &rc, hBgBrush);
        DeleteObject(hBgBrush);

        // Draw border (#1e232d)
        HPEN hBorderPen = CreatePen(PS_SOLID, 1, RGB(30, 35, 45));
        HGDIOBJ hOldPen = SelectObject(hdcMem, hBorderPen);
        MoveToEx(hdcMem, 0, 0, NULL);
        LineTo(hdcMem, rc.right - 1, 0);
        LineTo(hdcMem, rc.right - 1, rc.bottom - 1);
        LineTo(hdcMem, 0, rc.bottom - 1);
        LineTo(hdcMem, 0, 0);
        SelectObject(hdcMem, hOldPen);
        DeleteObject(hBorderPen);

        // Fonts
        HFONT hTitleFont = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
        HGDIOBJ hOldFont = SelectObject(hdcMem, hTitleFont);
        SetTextColor(hdcMem, RGB(243, 244, 246));
        SetBkMode(hdcMem, TRANSPARENT);
        
        RECT rcTitle = { 20, 20, rc.right - 100, 45 };
        DrawTextW(hdcMem, L"智眸录屏", -1, &rcTitle, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        // Status Text
        HFONT hStatusFont = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
        SelectObject(hdcMem, hStatusFont);
        SetTextColor(hdcMem, RGB(156, 163, 175));
        RECT rcStatus = { 20, 45, rc.right - 20, 70 };
        DrawTextW(hdcMem, g_splashStatusText.c_str(), -1, &rcStatus, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        // Progress bar track
        RECT rcProgressTrack = { 20, 80, rc.right - 20, 92 };
        HBRUSH hTrackBrush = CreateSolidBrush(RGB(22, 26, 36));
        FillRect(hdcMem, &rcProgressTrack, hTrackBrush);
        DeleteObject(hTrackBrush);

        // Progress bar fill (Accent Cyan #00f0f0)
        int fillWidth = (rcProgressTrack.right - rcProgressTrack.left) * g_splashProgress / 100;
        if (fillWidth > 0) {
            RECT rcProgressFill = { rcProgressTrack.left, rcProgressTrack.top, rcProgressTrack.left + fillWidth, rcProgressTrack.bottom };
            HBRUSH hFillBrush = CreateSolidBrush(RGB(0, 240, 240));
            FillRect(hdcMem, &rcProgressFill, hFillBrush);
            DeleteObject(hFillBrush);
        }

        // Percentage text
        wchar_t percentStr[32];
        swprintf_s(percentStr, L"%d%%", g_splashProgress);
        RECT rcPercent = { rc.right - 80, 20, rc.right - 20, 45 };
        SelectObject(hdcMem, hTitleFont);
        SetTextColor(hdcMem, RGB(0, 240, 240));
        DrawTextW(hdcMem, percentStr, -1, &rcPercent, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

        SelectObject(hdcMem, hOldFont);
        DeleteObject(hTitleFont);
        DeleteObject(hStatusFont);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, hOldObj);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);

        EndPaint(hWnd, &ps);
        return 0;
    }
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
}

void UpdateSplash(int progress, const std::wstring& statusText) {
    g_splashProgress = progress;
    g_splashStatusText = statusText;
    if (g_hSplashWnd) {
        InvalidateRect(g_hSplashWnd, NULL, FALSE);
        UpdateWindow(g_hSplashWnd);
        MSG msg;
        while (PeekMessage(&msg, g_hSplashWnd, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
}

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

static bool ExtractResource(int resourceId, const std::wstring& targetPath, bool forceOverwrite = false, bool isFfmpeg = false) {
    HRSRC hRes = FindResourceW(g_hInstance, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!hRes) return false;

    HGLOBAL hData = LoadResource(g_hInstance, hRes);
    if (!hData) return false;

    DWORD size = SizeofResource(g_hInstance, hRes);
    const BYTE* pData = (const BYTE*)LockResource(hData);
    if (!pData) return false;

    if (!forceOverwrite) {
        // Size-check first (fast path)
        HANDLE hCheck = CreateFileW(targetPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hCheck != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER fileSize;
            if (GetFileSizeEx(hCheck, &fileSize) && fileSize.QuadPart == size) {
                CloseHandle(hCheck);
                
                // If is Ffmpeg, report status and double-check MD5
                if (isFfmpeg) {
                    UpdateSplash(50, L"正在校验录屏引擎...");
                }
                std::string fileMd5 = GetFileMD5(targetPath);
                std::string resMd5 = GetBufferMD5(pData, size);
                if (fileMd5 == resMd5) {
                    return true; // Match, skip extraction!
                }
            } else {
                CloseHandle(hCheck);
            }
        }
    }

    // Write file in chunk-by-chunk loops
    HANDLE hFile = CreateFileW(targetPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    const DWORD CHUNK_SIZE = 1024 * 1024 * 4; // 4MB chunks
    DWORD bytesWrittenTotal = 0;
    
    while (bytesWrittenTotal < size) {
        DWORD chunkToWrite = (size - bytesWrittenTotal > CHUNK_SIZE) ? CHUNK_SIZE : (size - bytesWrittenTotal);
        DWORD bytesWritten = 0;
        if (!WriteFile(hFile, pData + bytesWrittenTotal, chunkToWrite, &bytesWritten, NULL) || bytesWritten != chunkToWrite) {
            CloseHandle(hFile);
            return false;
        }
        bytesWrittenTotal += bytesWritten;

        if (isFfmpeg) {
            int progress = (int)((ULONGLONG)bytesWrittenTotal * 100 / size);
            wchar_t status[128];
            swprintf_s(status, L"正在释放录屏引擎... (%d MB / %d MB)", bytesWrittenTotal / (1024 * 1024), size / (1024 * 1024));
            UpdateSplash(progress, status);
        }
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
    ok &= ExtractResource(IDR_FFMPEG_EXE, rootDir + L"\\ffmpeg.exe", false, true); // Do not force overwrite FFmpeg (huge file), but enable progress and MD5 verification
    ok &= ExtractResource(IDR_HTML, rootDir + L"\\ui\\index.html", true, false);    // Force overwrite UI files so updates take effect
    ok &= ExtractResource(IDR_CSS, rootDir + L"\\ui\\style.css", true, false);
    ok &= ExtractResource(IDR_JS, rootDir + L"\\ui\\main.js", true, false);
    return ok;
}

// Mode Switching Functions
void SwitchToCompactMode(int x, int y, int w, int h) {
    if (g_isCompact) return;

    // Save current window position
    GetWindowRect(g_hWnd, &g_normalRect);

    g_isCompact = true;

    UINT dpi = GetDpiForWindow(g_hWnd);
    if (dpi == 0) dpi = 96;

    // Compact dimensions: 380w x 80h (DPI scaled)
    int tbW = MulDiv(380, dpi, 96);
    int tbH = MulDiv(80, dpi, 96);
    int tbX = x + (w - tbW) / 2;
    int tbY = y + h + 12;

    // Use virtual screen metrics to correctly handle multi-monitor setups
    int vsLeft  = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vsTop   = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsRight = vsLeft + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vsBot   = vsTop  + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (tbX < vsLeft) tbX = vsLeft;
    if (tbX + tbW > vsRight) tbX = vsRight - tbW;
    if (tbY + tbH > vsBot) {
        tbY = y - tbH - 12; // Try placing above selected region
        if (tbY < vsTop) tbY = y + 10;
    }

    // Place window and make it topmost
    SetWindowPos(g_hWnd, HWND_TOPMOST, tbX, tbY, tbW, tbH, SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    // Create selection border outline window
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

    // Destroy outline border
    g_border.Destroy();

    g_isCompact = false;

    // Restore to saved position, clear TOPMOST
    // Use a safe initial height; JS ResizeObserver will re-measure and correct it
    int savedW = g_normalRect.right  - g_normalRect.left;
    int savedH = g_normalRect.bottom - g_normalRect.top;

    // Clamp saved position back onto any currently connected monitor
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

    // Ask JS to re-measure and send correct height back
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

// Helper function definitions
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
    int x;
    int y;
    int w;
    int h;
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

// JSON parsing helpers
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

// Get the user's default Videos path
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

// Send message to WebView2
void SendWebMessage(const std::string& message) {
    if (g_webView) {
        std::wstring wMsg = StringToWString(message);
        g_webView->PostWebMessageAsString(wMsg.c_str());
    }
}

// Handle WebView2 IPC messages
void HandleWebMessage(const std::string& message) {
    std::string action = GetJsonStringValue(message, "action");

    if (action == "get_init_status") {
        if (g_isInitComplete && !g_initStatusJson.empty()) {
            SendWebMessage(g_initStatusJson);
        }
    }
    else if (action == "init") {
        // Enumerate microphones
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
        // Hide main window during selection
        ShowWindow(g_hWnd, SW_MINIMIZE);
        
        // Block and show select overlay
        CropRect rect = SelectionOverlay::Show(g_hInstance);
        
        // Restore main window
        ShowWindow(g_hWnd, SW_RESTORE);
        SetForegroundWindow(g_hWnd);

        std::ostringstream ss;
        if (rect.cancelled) {
            ss << "{\"action\":\"select_area_response\",\"cancelled\":true}";
        } else {
            // Save coordinates
            g_cropX = rect.x;
            g_cropY = rect.y;
            g_cropW = rect.w;
            g_cropH = rect.h;

            ss << "{\"action\":\"select_area_response\",\"cancelled\":false"
               << ",\"x\":" << rect.x << ",\"y\":" << rect.y 
               << ",\"w\":" << rect.w << ",\"h\":" << rect.h << "}";
            
            // Switch to compact mode and display outline border wnd
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
            // Set border window color to red (recording)
            g_border.SetRecording(true);

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
            // Revert border recording color, and return window to normal layout
            g_border.SetRecording(false);
            SwitchToNormalMode();

            ss << "{\"action\":\"stop_recording_response\",\"success\":true}";
        } else {
            ss << "{\"action\":\"stop_recording_response\",\"success\":false,\"error\":\"" 
               << EscapeJsonString(error) << "\"}";
        }
        SendWebMessage(ss.str());
    }
    else if (action == "start_drag") {
        ReleaseCapture();
        SendMessage(g_hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
    else if (action == "cancel_selection") {
        SwitchToNormalMode();
    }
    else if (action == "minimize") {
        // Hide to system tray instead of minimizing to taskbar
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
        std::ostringstream ss;
        ss << "C++: Received resize_window, h=" << h << ", g_isCompact=" << g_isCompact;
        LogDebug(ss.str());

        if (!g_isCompact && h > 0) {
            UINT dpi = GetDpiForWindow(g_hWnd);
            if (dpi == 0) dpi = 96;

            int physicalWidth = MulDiv(620, dpi, 96);
            int physicalHeight = MulDiv(h, dpi, 96) + 4;

            std::ostringstream ss2;
            ss2 << "C++: SetWindowPos called, physicalWidth=" << physicalWidth 
                << ", physicalHeight=" << physicalHeight << " (DPI=" << dpi << ")";
            LogDebug(ss2.str());

            SetWindowPos(g_hWnd, NULL, 0, 0, physicalWidth, physicalHeight, 
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }
    else if (action == "log") {
        std::string logMsg = GetJsonStringValue(message, "message");
        LogDebug("JS_LOG: " + logMsg);
    }
}

// Window Procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCCALCSIZE: {
        if (wParam == TRUE) {
            // Returning 0 removes the default non-client frame (titlebar and borders),
            // while preserving the WS_THICKFRAME resize boundaries.
            return 0;
        }
        break;
    }
    case WM_DPICHANGED: {
        RECT* const prcNewWindow = (RECT*)lParam;
        UINT dpi = GetDpiForWindow(hWnd);
        std::ostringstream ss;
        ss << "C++: WM_DPICHANGED, recommended physical rect: L=" << prcNewWindow->left 
           << ", T=" << prcNewWindow->top 
           << ", R=" << prcNewWindow->right 
           << ", B=" << prcNewWindow->bottom 
           << " (w=" << (prcNewWindow->right - prcNewWindow->left) 
           << ", h=" << (prcNewWindow->bottom - prcNewWindow->top) 
           << "), new DPI=" << dpi;
        LogDebug(ss.str());

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
            // Set minimum window size in normal mode
            pInfo->ptMinTrackSize.x = MulDiv(500, dpi, 96);
            pInfo->ptMinTrackSize.y = MulDiv(400, dpi, 96);
        }
        return 0;
    }
    case WM_SIZE: {
        int width = LOWORD(lParam);
        int height = HIWORD(lParam);
        std::ostringstream ss;
        ss << "C++: WM_SIZE triggered, clientWidth=" << width << ", clientHeight=" << height;
        LogDebug(ss.str());

        if (g_webController != nullptr) {
            RECT bounds;
            GetClientRect(hWnd, &bounds);
            g_webController->put_Bounds(bounds);
        }
        break;
    }
    case WM_SYSCOMMAND: {
        // Intercept minimize: hide to tray instead
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

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow) {
    // Enable Per-Monitor DPI Awareness (V2) to ensure coordinates align across monitors with different scaling
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_hInstance = hInstance;

    // Clear old debug logs
    wchar_t myVideosPath[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_MYVIDEO, NULL, 0, myVideosPath);
    std::wstring logPath = std::wstring(myVideosPath) + L"\\resize_debug_log.txt";
    DeleteFileW(logPath.c_str());

    LogDebug("=== ScreenRecorder Debug Session Started ===");

    // Initialize COM for UI thread
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 1;

    // Register Window Class
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEX) };
    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.hInstance      = hInstance;
    wcex.hCursor        = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground  = CreateSolidBrush(RGB(12, 14, 18)); // Prevent white flash at startup
    wcex.lpszClassName  = L"ScreenRecorderMainClass";
    wcex.hIcon          = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wcex.hIconSm        = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    RegisterClassExW(&wcex);

    // Get DPI of primary monitor to scale initial window dimensions
    UINT dpi = GetDpiForSystem();
    if (dpi == 0) dpi = 96;

    int winWidth = MulDiv(620, dpi, 96);
    int winHeight = MulDiv(580, dpi, 96);
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenWidth - winWidth) / 2;
    int posY = (screenHeight - winHeight) / 2;

    // Create and Show Window IMMEDIATELY for sub-50ms instant UI startup!
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

    // Launch background thread to extract resources and probe GPU encoders asynchronously
    CreateThread(NULL, 0, [](LPVOID) -> DWORD {
        ExtractAllResources();
        g_recorder.Init();
        PostMessage(g_hWnd, WM_USER_INIT_DONE, 0, 0);
        return 0;
    }, NULL, 0, NULL);

    // Initialize WebView2
    // User data folder goes into AppData\ScreenRecorder\webview_cache
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

                            // Resize WebView to fit window
                            RECT bounds;
                            GetClientRect(g_hWnd, &bounds);
                            g_webController->put_Bounds(bounds);

                            // Get WebView interface
                            g_webController->get_CoreWebView2(&g_webView);
                            g_webView->AddRef();

                            // Settings customization
                            ICoreWebView2Settings* settings = nullptr;
                            g_webView->get_Settings(&settings);
                            if (settings != nullptr) {
                                settings->put_AreDevToolsEnabled(FALSE); // Disable F12 devtools for clean production UI
                                settings->put_AreDefaultContextMenusEnabled(FALSE); // Disable right click menus
                                settings->put_IsZoomControlEnabled(FALSE); // Disable pinch/ctrl zoom
                            }

                            // Register Message Received Callback
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

                            // Navigate to extracted index.html in AppData
                            std::wstring rootDir = GetAppDataRootDir();
                            std::wstring htmlPath = L"file:///" + rootDir + L"/ui/index.html";
                            // Replace backslashes with forward slashes for correct URL
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

    // Main message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup WebView2 resources
    if (g_webView) g_webView->Release();
    if (g_webController) g_webController->Release();

    CoUninitialize();
    return (int)msg.wParam;
}
