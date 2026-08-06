#include "selection_overlay.h"
#include <windowsx.h>

/// 内部蒙版绘制与拖拽状态结构
struct OverlayState {
    POINT startPoint;   // 拖拽起始点（客户区坐标）
    POINT endPoint;     // 拖拽终点（客户区坐标）
    bool dragging = false; // 是否正在拖拽中
    CropRect result;    // 最终选区结果
    bool hasSelection = false; // 是否已有有效画框选区
};

static OverlayState g_state;

CropRect SelectionOverlay::Show(HINSTANCE hInstance) {
    // 重置状态
    g_state = OverlayState();

    // 注册全屏蒙版窗口类
    WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"SelectionOverlayClass";
    wc.hCursor = LoadCursor(NULL, IDC_CROSS); // 十字光标
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

    RegisterClassEx(&wc);

    // 获取虚拟屏幕几何参数（完美适配多显示器/跨屏组合）
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

    // 设置分层窗口透明度：
    // 洋红色 RGB(255, 0, 255) 为完全透光的 ColorKey（用于抠出选中的明亮区域）
    // 整体 Alpha 不透明度为 130，实现周围未选中区域半透明暗化压暗的效果
    SetLayeredWindowAttributes(hwnd, RGB(255, 0, 255), 130, LWA_COLORKEY | LWA_ALPHA);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // 模态消息循环，等待用户完成拖拽或按 ESC 取消
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
        // 鼠标左键按下，开始捕获起始坐标
        g_state.startPoint.x = GET_X_LPARAM(lParam);
        g_state.startPoint.y = GET_Y_LPARAM(lParam);
        g_state.endPoint = g_state.startPoint;
        g_state.dragging = true;
        g_state.hasSelection = true;
        SetCapture(hwnd); // 捕获鼠标光标，防止拖拽超出窗口失效
        return 0;
    }
    case WM_MOUSEMOVE: {
        // 鼠标移动，更新终点坐标并触发重绘选区框
        if (g_state.dragging) {
            g_state.endPoint.x = GET_X_LPARAM(lParam);
            g_state.endPoint.y = GET_Y_LPARAM(lParam);
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        // 鼠标左键抬起，完成区域选择
        if (g_state.dragging) {
            g_state.endPoint.x = GET_X_LPARAM(lParam);
            g_state.endPoint.y = GET_Y_LPARAM(lParam);
            g_state.dragging = false;
            ReleaseCapture();

            // 转换窗口坐标至真实虚拟屏幕绝对坐标
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);

            int x1 = g_state.startPoint.x + vx;
            int y1 = g_state.startPoint.y + vy;
            int x2 = g_state.endPoint.x + vx;
            int y2 = g_state.endPoint.y + vy;

            g_state.result.x = min(x1, x2);
            g_state.result.y = min(y1, y2);
            g_state.result.w = abs(x2 - x1);
            g_state.result.h = abs(y2 - y1);
            // 选取小于 10 像素视作误触取消
            g_state.result.cancelled = (g_state.result.w < 10 || g_state.result.h < 10);

            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        // ESC 键取消选区
        if (wParam == VK_ESCAPE) {
            g_state.result.cancelled = true;
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        // 鼠标右键点击取消选区
        g_state.result.cancelled = true;
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT clientRect;
        GetClientRect(hwnd, &clientRect);

        // 使用双缓冲避免全屏拉拽时的刷屏闪烁
        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbmMem = CreateCompatibleBitmap(hdc, clientRect.right, clientRect.bottom);
        HGDIOBJ hOldObj = SelectObject(hdcMem, hbmMem);

        // 1. 填充半透明暗黑色背景
        HBRUSH hBgBrush = CreateSolidBrush(RGB(20, 20, 20));
        FillRect(hdcMem, &clientRect, hBgBrush);
        DeleteObject(hBgBrush);

        // 2. 绘制拖拽选区框
        if (g_state.hasSelection) {
            RECT selRect;
            selRect.left = min(g_state.startPoint.x, g_state.endPoint.x);
            selRect.top = min(g_state.startPoint.y, g_state.endPoint.y);
            selRect.right = max(g_state.startPoint.x, g_state.endPoint.x);
            selRect.bottom = max(g_state.startPoint.y, g_state.endPoint.y);

            // 用洋红色画刷填充选区内部，经过 ColorKey 作用后呈现完全透明无遮挡
            HBRUSH hKeyBrush = CreateSolidBrush(RGB(255, 0, 255));
            FillRect(hdcMem, &selRect, hKeyBrush);
            DeleteObject(hKeyBrush);

            // 绘制虚线青色高亮边框
            HPEN hPen = CreatePen(PS_DASH, 2, RGB(0, 240, 240));
            HGDIOBJ hOldPen = SelectObject(hdcMem, hPen);
            HGDIOBJ hOldBrush = SelectObject(hdcMem, GetStockObject(NULL_BRUSH));

            Rectangle(hdcMem, selRect.left, selRect.top, selRect.right, selRect.bottom);

            SelectObject(hdcMem, hOldBrush);
            SelectObject(hdcMem, hOldPen);
            DeleteObject(hPen);

            // 绘制当前框选的分辨率尺寸标注文本 (如 1920 x 1080)
            wchar_t szRes[128];
            wsprintf(szRes, L"%d x %d", abs(selRect.right - selRect.left), abs(selRect.bottom - selRect.top));
            SetTextColor(hdcMem, RGB(0, 240, 240));
            SetBkMode(hdcMem, TRANSPARENT);
            
            RECT textRect = selRect;
            textRect.bottom = selRect.top - 5;
            textRect.top = selRect.top - 25;
            if (textRect.top < 10) {
                textRect.top = selRect.top + 5;
                textRect.bottom = selRect.top + 25;
            }
            DrawText(hdcMem, szRes, -1, &textRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }

        // 3. 屏幕中央绘制提示操作文案
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

        // 将双缓冲画板 Blit 传输至屏幕 DC
        BitBlt(hdc, 0, 0, clientRect.right, clientRect.bottom, hdcMem, 0, 0, SRCCOPY);

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
