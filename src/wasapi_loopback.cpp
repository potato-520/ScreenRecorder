#include "wasapi_loopback.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <chrono>

// Explicitly define KSDATAFORMAT GUIDs if needed
#ifndef KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
#endif

#pragma pack(push, 1)
struct WAVHeader {
    char chunkId[4] = {'R', 'I', 'F', 'F'};
    uint32_t chunkSize = 0;
    char format[4] = {'W', 'A', 'V', 'E'};
    char subchunk1Id[4] = {'f', 'm', 't', ' '};
    uint32_t subchunk1Size = 16;
    uint16_t audioFormat = 1; // 1 = PCM, 3 = IEEE Float
    uint16_t numChannels = 2;
    uint32_t sampleRate = 48000;
    uint32_t byteRate = 192000;
    uint16_t blockAlign = 4;
    uint16_t bitsPerSample = 16;
    char subchunk2Id[4] = {'d', 'a', 't', 'a'};
    uint32_t subchunk2Size = 0;
};
#pragma pack(pop)

WASAPILoopback::WASAPILoopback() {}

WASAPILoopback::~WASAPILoopback() {
    Stop();
}

bool WASAPILoopback::Start(HANDLE hPipe) {
    if (m_running) return false;

    m_hPipe = hPipe;
    m_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!m_hStopEvent) return false;

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

    if (m_hStopEvent) {
        SetEvent(m_hStopEvent);
    }

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
    // Initialize COM on this thread
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) return;

    IMMDeviceEnumerator* pEnumerator = NULL;
    IMMDevice* pDevice = NULL;
    IAudioClient* pAudioClient = NULL;
    IAudioCaptureClient* pCaptureClient = NULL;
    WAVEFORMATEX* pwfx = NULL;

    do {
        // 1. Get MMDeviceEnumerator
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), NULL,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
            (void**)&pEnumerator
        );
        if (FAILED(hr)) break;

        // 2. Get default render endpoint (speaker loopback)
        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
        if (FAILED(hr)) break;

        // 3. Activate IAudioClient
        hr = pDevice->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient
        );
        if (FAILED(hr)) break;

        // 4. Get current mix format
        hr = pAudioClient->GetMixFormat(&pwfx);
        if (FAILED(hr)) break;

        // 5. Initialize client in loopback mode
        REFERENCE_TIME hnsRequestedDuration = 10000000;
        hr = pAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            hnsRequestedDuration,
            0,
            pwfx,
            NULL
        );
        if (FAILED(hr)) break;

        // 6. Get IAudioCaptureClient
        hr = pAudioClient->GetService(
            __uuidof(IAudioCaptureClient), (void**)&pCaptureClient
        );
        if (FAILED(hr)) break;

        // Wait for connection to the named pipe (FFmpeg to open it)
        if (m_hPipe != INVALID_HANDLE_VALUE) {
            ConnectNamedPipe(m_hPipe, NULL);
        }

        // 8. Start recording
        hr = pAudioClient->Start();
        if (FAILED(hr)) break;

        auto lastWriteTime = std::chrono::steady_clock::now();

        // 9. Loop until stopped
        while (WaitForSingleObject(m_hStopEvent, 10) == WAIT_TIMEOUT) {
            UINT32 packetLength = 0;
            hr = pCaptureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;

            bool wroteData = false;
            bool pipeBroken = false;

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
                        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                            std::vector<BYTE> silence(bytesToWrite, 0);
                            ok = WriteFile(m_hPipe, silence.data(), bytesToWrite, &bytesWritten, NULL);
                        } else {
                            ok = WriteFile(m_hPipe, pData, bytesToWrite, &bytesWritten, NULL);
                        }
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
                // If WASAPI yielded no packets (e.g. system audio is silent), write silence corresponding to elapsed time
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

        // 10. Stop recording
        pAudioClient->Stop();

    } while (false);

    // Clean up
    if (pwfx) CoTaskMemFree(pwfx);
    if (pCaptureClient) pCaptureClient->Release();
    if (pAudioClient) pAudioClient->Release();
    if (pDevice) pDevice->Release();
    if (pEnumerator) pEnumerator->Release();

    CoUninitialize();
}
