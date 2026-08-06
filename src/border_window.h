#pragma once

#include <windows.h>
#include <functional>

class RecordBorder {
public:
    using AreaChangedCallback = std::function<void(int x, int y, int w, int h)>;

    RecordBorder();
    ~RecordBorder();

    // Create the border window outlining the target coordinates
    bool Create(HINSTANCE hInstance, int x, int y, int w, int h);

    // Set callback when user moves or resizes the recording area
    void SetOnAreaChanged(AreaChangedCallback cb) { m_onAreaChanged = cb; }

    // Change border color (cyan when idle, red when recording)
    void SetRecording(bool recording);

    // Get current recording coordinates
    void GetArea(int& x, int& y, int& w, int& h) const;

    // Destroy the border window
    void Destroy();

    // Check if the border window is active
    bool IsActive() const { return m_hWnd != NULL; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void UpdateGeometryFromWindowRect();

private:
    HWND m_hWnd = NULL;
    bool m_recording = false;
    int m_cropX = 0;
    int m_cropY = 0;
    int m_cropW = 0;
    int m_cropH = 0;
    AreaChangedCallback m_onAreaChanged = nullptr;
};

