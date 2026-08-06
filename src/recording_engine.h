#pragma once

#include <string>
#include <vector>
#include <windows.h>
#include "wasapi_loopback.h"

struct AudioDevice {
    std::string id;
    std::string name;
};

class RecordingEngine {
public:
    RecordingEngine();
    ~RecordingEngine();

    // Initialize the engine: detects ffmpeg.exe and GPU encoders
    bool Init();

    // Enumerate active microphone devices using WASAPI
    std::vector<AudioDevice> GetMicrophoneDevices();

    // Start screen recording with the specified parameters
    bool StartRecording(
        int x, int y, int width, int height,
        bool recordMic, const std::string& micDeviceName,
        bool recordSysAudio,
        const std::string& outputFolder,
        std::string& outFilename,
        std::string& outError
    );

    // Stop recording gracefully (sends 'q' to FFmpeg)
    bool StopRecording(std::string& outError);

    // Check if recording is running
    bool IsRecording() const { return m_recording; }

    // Get the name of the auto-detected best encoder
    std::string GetBestEncoderName() const { return m_bestEncoder; }
    std::string GetBestEncoderFriendlyName() const { return m_bestEncoderFriendly; }

private:
    // Helper to probe an encoder by running a short FFmpeg command
    bool ProbeEncoder(const std::string& encoderName);
    
    // Helper to auto-detect the best encoder using DXGI and FFmpeg probes
    void DetectBestEncoder();

    // Helper to escape special characters or format FFmpeg commands
    std::string GetFFmpegPath() const;

private:
    bool m_initialized = false;
    bool m_recording = false;
    std::string m_bestEncoder = "libx264"; // Default fallback
    std::string m_bestEncoderFriendly = "CPU (x264)";
    std::string m_ffmpegPath;

    // Native audio loopback recorder and Named Pipe handle
    WASAPILoopback m_loopback;
    HANDLE m_hAudioPipe = INVALID_HANDLE_VALUE;
    bool m_recordSysAudio = false;
    bool m_hasMicInput = false;
    std::string m_finalOutputPath;

    // Process variables for ffmpeg
    PROCESS_INFORMATION m_pi = {};
    HANDLE m_hStdInWrite = nullptr; // Pipe to write 'q' to FFmpeg
};
