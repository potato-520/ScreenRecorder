#pragma once

#include <string>
#include <windows.h>

/**
 * @brief 原生 WASAPI 系统音频回放采集类 (WASAPILoopback)
 * 
 * 采用 Windows Audio Session API (WASAPI) 硬件级 Loopback 模式，
 * 实时抓取系统声卡输出的数字音频流，并通过命名管道实时推送到 FFmpeg 编码器。
 */
class WASAPILoopback {
public:
    WASAPILoopback();
    ~WASAPILoopback();

    /**
     * @brief 启动后台 WASAPI 音频采集线程
     * @param hPipe 用于传输 PCM 数据的命名管道句柄
     * @return 成功返回 true，失败返回 false
     */
    bool Start(HANDLE hPipe);

    /**
     * @brief 停止音频采集并关闭线程
     */
    void Stop();

    /**
     * @brief 检查音频采集是否正在运行
     */
    bool IsRunning() const { return m_running; }

private:
    /// WINAPI 采集线程入口函数
    static DWORD WINAPI LoopbackThread(LPVOID param);
    
    /// WASAPI 采集主循环逻辑（含静音自动补帧防护）
    void RunLoopback();

private:
    HANDLE m_hPipe = INVALID_HANDLE_VALUE;  // 命名管道句柄
    HANDLE m_hThread = NULL;                // 采集线程句柄
    HANDLE m_hStopEvent = NULL;             // 停止信号事件
    bool m_running = false;                 // 运行状态标识
};
