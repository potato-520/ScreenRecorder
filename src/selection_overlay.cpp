#include "selection_overlay.h"
#include <windowsx.h>

struct OverlayState {
    POINT startPoint;
    POINT endPoint;
    bool dragging = false;
    CropRect result;
    bool hasSelection = false;
};

static OverlayState g_state;

CropRect SelectionOverlay::Show(HINSTANCE hInstance) {
    // Reset global state
    g_state = OverlayState();

    // Register class
    WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"SelectionOverlayClass";
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

    RegisterClassEx(&wc);

    // Get virtual screen dimensions (handles multi-monitor setups)
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HWND hwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"SelectionOverlayClass",
        L"Select Recording Area",
        WS_POPUP,
        x, y, w, h,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) {
        UnregisterClass(L"SelectionOverlayClass", hInstance);
        g_state.result.cancelled = true;
        return g_state.result;
    }

    // Set transparency and color key
    // RGB(255, 0, 255) [Magenta] will be completely transparent/clear
    // Global alpha 130 (out of 255) will dim the rest of the screen
    SetLayeredWindowAttributes(hwnd, RGB(255, 0, 255), 130, LWA_COLORKEY | LWA_ALPHA);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Modal message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnregisterClass(L"SelectionOverlayClass", hInstance);
    return g_state.result;
}

LRESULT CALLBACK SelectionOverlay::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_LBUTTONDOWN: {
        g_state.startPoint.x = GET_X_LPARAM(lParam);
        g_state.startPoint.y = GET_Y_LPARAM(lParam);
        g_state.endPoint = g_state.startPoint;
        g_state.dragging = true;
        g_state.hasSelection = true;
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_state.dragging) {
            g_state.endPoint.x = GET_X_LPARAM(lParam);
            g_state.endPoint.y = GET_Y_LPARAM(lParam);
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_state.dragging) {
            g_state.endPoint.x = GET_X_LPARAM(lParam);
            g_state.endPoint.y = GET_Y_LPARAM(lParam);
            g_state.dragging = false;
            ReleaseCapture();

            // Calculate final rectangle (convert to screen coordinates relative to virtual screen)
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);

            // Screen coordinates are window coordinates + virtual screen offset
            int x1 = g_state.startPoint.x + vx;
            int y1 = g_state.startPoint.y + vy;
            int x2 = g_state.endPoint.x + vx;
            int y2 = g_state.endPoint.y + vy;

            g_state.result.x = min(x1, x2);
            g_state.result.y = min(y1, y2);
            g_state.result.w = abs(x2 - x1);
            g_state.result.h = abs(y2 - y1);
            g_state.result.cancelled = (g_state.result.w < 10 || g_state.result.h < 10);

            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (wParam == VK_ESCAPE) {
            g_state.result.cancelled = true;
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        g_state.result.cancelled = true;
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        // Get window client dimensions
        RECT clientRect;
        GetClientRect(hwnd, &clientRect);

        // Create a double buffer to prevent flickering
        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbmMem = CreateCompatibleBitmap(hdc, clientRect.right, clientRect.bottom);
        HGDIOBJ hOldObj = SelectObject(hdcMem, hbmMem);

        // 1. Fill background with semi-transparent black
        // Since the window is layered with alpha 130, this black brush will create the dimming effect
        HBRUSH hBgBrush = CreateSolidBrush(RGB(20, 20, 20));
        FillRect(hdcMem, &clientRect, hBgBrush);
        DeleteObject(hBgBrush);

        // 2. Draw selection area
        if (g_state.hasSelection) {
            RECT selRect;
            selRect.left = min(g_state.startPoint.x, g_state.endPoint.x);
            selRect.top = min(g_state.startPoint.y, g_state.endPoint.y);
            selRect.right = max(g_state.startPoint.x, g_state.endPoint.x);
            selRect.bottom = max(g_state.startPoint.y, g_state.endPoint.y);

            // Fill selected rectangle with the colorkey (Magenta) to make it transparent
            HBRUSH hKeyBrush = CreateSolidBrush(RGB(255, 0, 255));
            FillRect(hdcMem, &selRect, hKeyBrush);
            DeleteObject(hKeyBrush);

            // Draw cyan border around selection
            HPEN hPen = CreatePen(PS_DASH, 2, RGB(0, 240, 240));
            HGDIOBJ hOldPen = SelectObject(hdcMem, hPen);
            HGDIOBJ hOldBrush = SelectObject(hdcMem, GetStockObject(NULL_BRUSH));

            Rectangle(hdcMem, selRect.left, selRect.top, selRect.right, selRect.bottom);

            SelectObject(hdcMem, hOldBrush);
            SelectObject(hdcMem, hOldPen);
            DeleteObject(hPen);

            // Draw resolution overlay text near selection
            wchar_t szRes[128];
            wsprintf(szRes, L"%d x %d", abs(selRect.right - selRect.left), abs(selRect.bottom - selRect.top));
            SetTextColor(hdcMem, RGB(0, 240, 240));
            SetBkMode(hdcMem, TRANSPARENT);
            
            // Positioning resolution text
            RECT textRect = selRect;
            textRect.bottom = selRect.top - 5;
            textRect.top = selRect.top - 25;
            if (textRect.top < 10) {
                // If it goes off the screen, place it inside the selection box
                textRect.top = selRect.top + 5;
                textRect.bottom = selRect.top + 25;
            }
            DrawText(hdcMem, szRes, -1, &textRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }

        // 3. Draw guiding text in the center of the screen
        SetTextColor(hdcMem, RGB(255, 255, 255));
        SetBkMode(hdcMem, TRANSPARENT);
        HFONT hFont = CreateFont(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Microsoft YaHei");
        HGDIOBJ hOldFont = SelectObject(hdcMem, hFont);

        RECT hintRect = clientRect;
        hintRect.top = clientRect.bottom / 2 - 100;
        hintRect.bottom = clientRect.bottom / 2 + 100;
        DrawText(hdcMem, L"按下鼠标左键并拖拽以选择录制区域\n按 ESC 或 鼠标右键 取消选择", -1, &hintRect, DT_CENTER | DT_WORDBREAK);

        SelectObject(hdcMem, hOldFont);
        DeleteObject(hFont);

        // Blit to screen
        BitBlt(hdc, 0, 0, clientRect.right, clientRect.bottom, hdcMem, 0, 0, SRCCOPY);

        // Clean up memory DC
        SelectObject(hdcMem, hOldObj);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY: {
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
