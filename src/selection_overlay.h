#pragma once

#include <windows.h>

struct CropRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool cancelled = false;
};

class SelectionOverlay {
public:
    // Display the overlay and block until selection is complete or cancelled
    static CropRect Show(HINSTANCE hInstance);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
};
