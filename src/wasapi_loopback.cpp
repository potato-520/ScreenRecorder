#include "wasapi_loopback.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <chrono>

// IEEE 浮点格式 GUID 定义
#ifndef KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
#endif

WASAPILoopback::WASAPILoopback() {}

WASAPILoopback::~WASAPILoopback() {
    Stop();
}

bool WASAPILoopback::Start(HANDLE hPipe) {
    if (m_running) return false;

    m_hPipe = hPipe;
    // 创建停止事件
    m_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!m_hStopEvent) return false;

    // 启动音频采集工作线程
    m_hThread = CreateThread(NULL, 0, LoopbackThread, this, 0, NULL);
    if (!m_hThread) {
        CloseHandle(m_hStopEvent);
        m_hStopEvent = NULL;
        return false;
    }

    m_running = true;
    return true;
}

void WASAPILoopback::Stop() {
    if (!m_running) return;

    // 触发停止信号
    if (m_hStopEvent) {
        SetEvent(m_hStopEvent);
    }

    // 等待后台采集线程优雅退出
    if (m_hThread) {
        WaitForSingleObject(m_hThread, INFINITE);
        CloseHandle(m_hThread);
        m_hThread = NULL;
    }

    if (m_hStopEvent) {
        CloseHandle(m_hStopEvent);
        m_hStopEvent = NULL;
    }

    m_running = false;
}

DWORD WINAPI WASAPILoopback::LoopbackThread(LPVOID param) {
    WASAPILoopback* pThis = (WASAPILoopback*)param;
    pThis->RunLoopback();
    return 0;
}

void WASAPILoopback::RunLoopback() {
    // 线程内初始化 COM 组件 (多线程 Apartment 模式)
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) return;

    IMMDeviceEnumerator* pEnumerator = NULL;
    IMMDevice* pDevice = NULL;
    IAudioClient* pAudioClient = NULL;
    IAudioCaptureClient* pCaptureClient = NULL;
    WAVEFORMATEX* pwfx = NULL;

    do {
        // 1. 创建音频设备枚举器
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), NULL,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
            (void**)&pEnumerator
        );
        if (FAILED(hr)) break;

        // 2. 获取默认系统音频渲染终端 (扬声器/耳机的 Loopback 回放端点)
        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
        if (FAILED(hr)) break;

        // 3. 激活 IAudioClient 音频客户端
        hr = pDevice->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient
        );
        if (FAILED(hr)) break;

        // 4. 获取当前声卡的混音格式 (SampleRate, Channels, BitsPerSample)
        hr = pAudioClient->GetMixFormat(&pwfx);
        if (FAILED(hr)) break;

        // 5. 初始化 WASAPILoopback 模式 (共享模式 + AUDCLNT_STREAMFLAGS_LOOPBACK)
        REFERENCE_TIME hnsRequestedDuration = 10000000; // 1秒缓冲
        hr = pAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            hnsRequestedDuration,
            0,
            pwfx,
            NULL
        );
        if (FAILED(hr)) break;

        // 6. 获取音频捕获客户端服务接口
        hr = pAudioClient->GetService(
            __uuidof(IAudioCaptureClient), (void**)&pCaptureClient
        );
        if (FAILED(hr)) break;

        // 7. 连接至命名管道（等待 FFmpeg 打开管道读取流）
        if (m_hPipe != INVALID_HANDLE_VALUE) {
            ConnectNamedPipe(m_hPipe, NULL);
        }

        // 8. 启动音频采集
        hr = pAudioClient->Start();
        if (FAILED(hr)) break;

        auto lastWriteTime = std::chrono::steady_clock::now();

        // 9. 循环采集 PCM 数据流，直到收到停止信号
        while (WaitForSingleObject(m_hStopEvent, 10) == WAIT_TIMEOUT) {
            UINT32 packetLength = 0;
            hr = pCaptureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;

            bool wroteData = false;
            bool pipeBroken = false;

            // 读取所有可用的音频数据包
            while (packetLength > 0) {
                BYTE* pData = NULL;
                UINT32 numFramesToRead = 0;
                DWORD flags = 0;

                hr = pCaptureClient->GetBuffer(
                    &pData, &numFramesToRead, &flags, NULL, NULL
                );
                if (FAILED(hr)) break;

                if (numFramesToRead > 0 && pData) {
                    UINT32 bytesToWrite = numFramesToRead * pwfx->nBlockAlign;

                    if (m_hPipe != INVALID_HANDLE_VALUE) {
                        DWORD bytesWritten = 0;
                        BOOL ok = FALSE;
                        
                        // 如果声卡标识当前包为静音包，填充全 0 的 PCM 字节；否则写入真实 PCM 数据
                        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                            std::vector<BYTE> silence(bytesToWrite, 0);
                            ok = WriteFile(m_hPipe, silence.data(), bytesToWrite, &bytesWritten, NULL);
                        } else {
                            ok = WriteFile(m_hPipe, pData, bytesToWrite, &bytesWritten, NULL);
                        }
                        
                        // 管道破裂（FFmpeg 退出）时及时退出循环
                        if (!ok) {
                            pipeBroken = true;
                            pCaptureClient->ReleaseBuffer(numFramesToRead);
                            break;
                        }
                        wroteData = true;
                    }
                }

                pCaptureClient->ReleaseBuffer(numFramesToRead);

                hr = pCaptureClient->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) break;
            }

            if (pipeBroken) break;

            auto now = std::chrono::steady_clock::now();
            if (wroteData) {
                lastWriteTime = now;
            } else {
                // 【核心防卡死机制】：当系统完全无声时，WASAPI 不会吐出数据包。
                // 此时按流逝的时间差主动向管道写入全 0 的静音 PCM 帧，保证 FFmpeg 音频流不饿死卡帧。
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastWriteTime).count();
                if (elapsedMs >= 10) {
                    uint32_t samplesNeeded = (uint32_t)(pwfx->nSamplesPerSec * elapsedMs / 1000);
                    if (samplesNeeded > 0) {
                        uint32_t bytesToWrite = samplesNeeded * pwfx->nBlockAlign;
                        std::vector<BYTE> silence(bytesToWrite, 0);
                        DWORD bytesWritten = 0;
                        if (m_hPipe != INVALID_HANDLE_VALUE) {
                            if (!WriteFile(m_hPipe, silence.data(), bytesToWrite, &bytesWritten, NULL)) {
                                break;
                            }
                        }
                        lastWriteTime = now;
                    }
                }
            }
        }

        // 10. 停止 WASAPI 采集客户端
        pAudioClient->Stop();

    } while (false);

    // 释放资源
    if (pwfx) CoTaskMemFree(pwfx);
    if (pCaptureClient) pCaptureClient->Release();
    if (pAudioClient) pAudioClient->Release();
    if (pDevice) pDevice->Release();
    if (pEnumerator) pEnumerator->Release();

    CoUninitialize();
}
