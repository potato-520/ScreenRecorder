#pragma once

#include <string>
#include <windows.h>

class WASAPILoopback {
public:
    WASAPILoopback();
    ~WASAPILoopback();

    // Start background loopback capture, writing to target named pipe
    bool Start(HANDLE hPipe);

    // Stop recording
    void Stop();

    // Check if recording is running
    bool IsRunning() const { return m_running; }

private:
    static DWORD WINAPI LoopbackThread(LPVOID param);
    void RunLoopback();

private:
    HANDLE m_hPipe = INVALID_HANDLE_VALUE;
    HANDLE m_hThread = NULL;
    HANDLE m_hStopEvent = NULL;
    bool m_running = false;
};
