#include "border_window.h"
#include <windowsx.h>
#include <cstdio>

static const int BORDER_MARGIN = 8;
static const int BADGE_HEIGHT = 26;

RecordBorder::RecordBorder() {}

RecordBorder::~RecordBorder() {
    Destroy();
}

bool RecordBorder::Create(HINSTANCE hInstance, int x, int y, int w, int h) {
    Destroy(); // Ensure clean state

    m_cropX = x;
    m_cropY = y;
    m_cropW = w;
    m_cropH = h;

    // Register class
    WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"RecordBorderClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);

    RegisterClassEx(&wc);

    int wx = x - BORDER_MARGIN;
    int wy = y - BORDER_MARGIN - BADGE_HEIGHT;
    int ww = w + 2 * BORDER_MARGIN;
    int wh = h + 2 * BORDER_MARGIN + BADGE_HEIGHT;

    m_hWnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"RecordBorderClass",
        L"Recording Area",
        WS_POPUP,
        wx, wy, ww, wh,
        NULL, NULL, hInstance, this
    );

    if (!m_hWnd) {
        return false;
    }

    SetLayeredWindowAttributes(m_hWnd, RGB(255, 0, 255), 255, LWA_COLORKEY);

    ShowWindow(m_hWnd, SW_SHOWNOACTIVATE);
    UpdateWindow(m_hWnd);

    return true;
}

void RecordBorder::SetRecording(bool recording) {
    if (m_hWnd) {
        m_recording = recording;
        InvalidateRect(m_hWnd, NULL, TRUE);
        UpdateWindow(m_hWnd);
    }
}

void RecordBorder::GetArea(int& x, int& y, int& w, int& h) const {
    x = m_cropX;
    y = m_cropY;
    w = m_cropW;
    h = m_cropH;
}

void RecordBorder::Destroy() {
    if (m_hWnd) {
        DestroyWindow(m_hWnd);
        m_hWnd = NULL;
    }
}

void RecordBorder::UpdateGeometryFromWindowRect() {
    if (!m_hWnd) return;

    RECT rc;
    GetWindowRect(m_hWnd, &rc);

    int newX = rc.left + BORDER_MARGIN;
    int newY = rc.top + BORDER_MARGIN + BADGE_HEIGHT;
    int newW = (rc.right - rc.left) - 2 * BORDER_MARGIN;
    int newH = (rc.bottom - rc.top) - 2 * BORDER_MARGIN - BADGE_HEIGHT;

    if (newW < 60) newW = 60;
    if (newH < 60) newH = 60;
    if (newW % 2 != 0) newW--;
    if (newH % 2 != 0) newH--;

    if (newX != m_cropX || newY != m_cropY || newW != m_cropW || newH != m_cropH) {
        m_cropX = newX;
        m_cropY = newY;
        m_cropW = newW;
        m_cropH = newH;

        if (m_onAreaChanged) {
            m_onAreaChanged(m_cropX, m_cropY, m_cropW, m_cropH);
        }
    }
}

LRESULT CALLBACK RecordBorder::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    RecordBorder* pThis = nullptr;
    if (uMsg == WM_NCCREATE) {
        CREATESTRUCT* pCreate = (CREATESTRUCT*)lParam;
        pThis = (RecordBorder*)pCreate->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)pThis);
    } else {
        pThis = (RecordBorder*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    }

    switch (uMsg) {
    case WM_NCHITTEST: {
        if (pThis && pThis->m_recording) {
            return HTTRANSPARENT;
        }

        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);

        RECT rcClient;
        GetClientRect(hwnd, &rcClient);
        int ww = rcClient.right;
        int wh = rcClient.bottom;

        int innerLeft = BORDER_MARGIN;
        int innerTop = BORDER_MARGIN + BADGE_HEIGHT;
        int innerRight = ww - BORDER_MARGIN;
        int innerBottom = wh - BORDER_MARGIN;

        // Interior area (where recording takes place): pass clicks through to windows below
        if (pt.x > innerLeft && pt.x < innerRight && pt.y > innerTop && pt.y < innerBottom) {
            return HTTRANSPARENT;
        }

        // Corner hit tests (16px corner zone for easy grabbing)
        int corner = 16;
        if (pt.x <= corner && pt.y <= innerTop + 8) return HTTOPLEFT;
        if (pt.x >= ww - corner && pt.y <= innerTop + 8) return HTTOPRIGHT;
        if (pt.x <= corner && pt.y >= wh - corner) return HTBOTTOMLEFT;
        if (pt.x >= ww - corner && pt.y >= wh - corner) return HTBOTTOMRIGHT;

        // Edge hit tests
        if (pt.y <= innerTop && pt.y > BADGE_HEIGHT) return HTTOP;
        if (pt.y >= innerBottom) return HTBOTTOM;
        if (pt.x <= innerLeft) return HTLEFT;
        if (pt.x >= innerRight) return HTRIGHT;

        // Top drag badge / bar
        return HTCAPTION;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* pMMI = (MINMAXINFO*)lParam;
        pMMI->ptMinTrackSize.x = 100 + 2 * BORDER_MARGIN;
        pMMI->ptMinTrackSize.y = 100 + 2 * BORDER_MARGIN + BADGE_HEIGHT;
        return 0;
    }
    case WM_WINDOWPOSCHANGED: {
        WINDOWPOS* pPos = (WINDOWPOS*)lParam;
        if (pThis && (!(pPos->flags & SWP_NOMOVE) || !(pPos->flags & SWP_NOSIZE))) {
            pThis->UpdateGeometryFromWindowRect();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rect;
        GetClientRect(hwnd, &rect);
        int ww = rect.right;
        int wh = rect.bottom;

        // Memory DC for flicker-free double buffering
        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbmMem = CreateCompatibleBitmap(hdc, ww, wh);
        HGDIOBJ hOldObj = SelectObject(hdcMem, hbmMem);

        // Fill background with colorkey (Magenta)
        HBRUSH hBgBrush = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(hdcMem, &rect, hBgBrush);
        DeleteObject(hBgBrush);

        bool rec = pThis && pThis->m_recording;
        COLORREF borderColor = rec ? RGB(239, 68, 68) : RGB(0, 240, 240);

        int rx = BORDER_MARGIN;
        int ry = BORDER_MARGIN + BADGE_HEIGHT;
        int rw = ww - 2 * BORDER_MARGIN;
        int rh = wh - 2 * BORDER_MARGIN - BADGE_HEIGHT;

        // 1. Draw 2px border around recording rectangle
        HPEN hPen = CreatePen(PS_SOLID, 2, borderColor);
        HGDIOBJ hOldPen = SelectObject(hdcMem, hPen);
        HGDIOBJ hOldBrush = SelectObject(hdcMem, GetStockObject(NULL_BRUSH));

        Rectangle(hdcMem, rx, ry, rx + rw, ry + rh);

        SelectObject(hdcMem, hOldBrush);
        SelectObject(hdcMem, hOldPen);
        DeleteObject(hPen);

        // 2. Draw 8 handle knobs when idle (!rec)
        if (!rec) {
            HBRUSH hKnobBrush = CreateSolidBrush(RGB(255, 255, 255));
            HPEN hKnobPen = CreatePen(PS_SOLID, 1, borderColor);
            SelectObject(hdcMem, hKnobBrush);
            SelectObject(hdcMem, hKnobPen);

            int ks = 6; // Knob size 6x6
            int kpts[8][2] = {
                { rx, ry },                         // Top-Left
                { rx + rw / 2, ry },                // Top-Center
                { rx + rw, ry },                    // Top-Right
                { rx + rw, ry + rh / 2 },           // Right-Center
                { rx + rw, ry + rh },               // Bottom-Right
                { rx + rw / 2, ry + rh },           // Bottom-Center
                { rx, ry + rh },                    // Bottom-Left
                { rx, ry + rh / 2 }                 // Left-Center
            };

            for (int i = 0; i < 8; i++) {
                Rectangle(hdcMem, kpts[i][0] - ks / 2, kpts[i][1] - ks / 2, kpts[i][0] + ks / 2 + 1, kpts[i][1] + ks / 2 + 1);
            }

            SelectObject(hdcMem, GetStockObject(NULL_BRUSH));
            SelectObject(hdcMem, GetStockObject(BLACK_PEN));
            DeleteObject(hKnobBrush);
            DeleteObject(hKnobPen);
        }

        // 3. Draw top Resolution Badge & Drag Bar
        wchar_t szBadge[64];
        int curW = (pThis ? pThis->m_cropW : rw);
        int curH = (pThis ? pThis->m_cropH : rh);
        if (rec) {
            swprintf_s(szBadge, L"  ● REC  %d × %d  ", curW, curH);
        } else {
            swprintf_s(szBadge, L"  ❖ 录制区域  %d × %d (按住拖拽/拉伸)  ", curW, curH);
        }

        HFONT hFont = CreateFont(
            13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei"
        );
        HGDIOBJ hOldFont = SelectObject(hdcMem, hFont);

        SIZE txtSize;
        GetTextExtentPoint32(hdcMem, szBadge, (int)wcslen(szBadge), &txtSize);

        int badgeW = txtSize.cx + 16;
        int badgeH = BADGE_HEIGHT - 2;
        int badgeX = rx;
        int badgeY = 0;

        // Draw badge dark background pill
        HBRUSH hBadgeBg = CreateSolidBrush(RGB(20, 24, 33));
        HPEN hBadgePen = CreatePen(PS_SOLID, 1, borderColor);
        SelectObject(hdcMem, hBadgeBg);
        SelectObject(hdcMem, hBadgePen);

        RoundRect(hdcMem, badgeX, badgeY, badgeX + badgeW, badgeY + badgeH, 6, 6);

        // Draw text
        SetBkMode(hdcMem, TRANSPARENT);
        SetTextColor(hdcMem, rec ? RGB(239, 68, 68) : RGB(0, 240, 240));

        RECT txtRect = { badgeX, badgeY, badgeX + badgeW, badgeY + badgeH };
        DrawText(hdcMem, szBadge, -1, &txtRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdcMem, hOldFont);
        DeleteObject(hFont);
        DeleteObject(hBadgeBg);
        DeleteObject(hBadgePen);

        // Copy memory DC to screen
        BitBlt(hdc, 0, 0, ww, wh, hdcMem, 0, 0, SRCCOPY);

        SelectObject(hdcMem, hOldObj);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

