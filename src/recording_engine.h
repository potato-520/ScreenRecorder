#pragma once

#include <string>
#include <vector>
#include <windows.h>
#include "wasapi_loopback.h"

/// 麦克风音频设备信息结构体
struct AudioDevice {
    std::string id;   // 设备唯一 ID
    std::string name; // 设备友好名称（如 "Realtek High Definition Audio"）
};

/**
 * @brief FFmpeg 屏幕录制引擎核心控制类 (RecordingEngine)
 * 
 * 负责自动探测显卡硬件加速编码器（NVENC / QSV / AMF / MF），
 * 拼接 FFmpeg 命令行参数，启动 FFmpeg 进行抓屏与音视频混音，
 * 并配合 WASAPILoopback 实现高质量屏幕与音频录制。
 */
class RecordingEngine {
public:
    RecordingEngine();
    ~RecordingEngine();

    /**
     * @brief 初始化录制引擎：定位 ffmpeg.exe 路径并探测最优硬件编码器
     * @return 成功返回 true，失败返回 false
     */
    bool Init();

    /**
     * @brief 枚举系统当前激活的麦克风输入设备列表
     */
    std::vector<AudioDevice> GetMicrophoneDevices();

    /**
     * @brief 启动屏幕录制
     * @param x 录制区域 X 坐标
     * @param y 录制区域 Y 坐标
     * @param width 录制区域宽度
     * @param height 录制区域高度
     * @param recordMic 是否录制麦克风
     * @param micDeviceName 目标麦克风设备名称
     * @param recordSysAudio 是否录制系统声音
     * @param outputFolder 保存目录
     * @param outFilename 传出生成的录像文件名
     * @param outError 传出错误信息
     * @return 启动成功返回 true，失败返回 false
     */
    bool StartRecording(
        int x, int y, int width, int height,
        bool recordMic, const std::string& micDeviceName,
        bool recordSysAudio,
        const std::string& outputFolder,
        std::string& outFilename,
        std::string& outError
    );

    /**
     * @brief 停止录制（通过标准输入 Pipe 向 FFmpeg 写入 'q' 键实现优雅退出）
     */
    bool StopRecording(std::string& outError);

    /**
     * @brief 检查当前是否正在录制
     */
    bool IsRecording() const { return m_recording; }

    /**
     * @brief 获取自动探测到的最优编码器标识（如 "h264_nvenc"）
     */
    std::string GetBestEncoderName() const { return m_bestEncoder; }

    /**
     * @brief 获取自动探测到的最优编码器友好名称（如 "NVIDIA NVENC (加速)"）
     */
    std::string GetBestEncoderFriendlyName() const { return m_bestEncoderFriendly; }

private:
    /// 通过运行 1 帧的测试指令探测目标编码器是否可用
    bool ProbeEncoder(const std::string& encoderName);
    
    /// 结合 DXGI 显卡 VendorID 与 FFmpeg 探测自动选择最优编码器
    void DetectBestEncoder();

    /// 获取 ffmpeg.exe 的实际可执行路径
    std::string GetFFmpegPath() const;

private:
    bool m_initialized = false;                         // 是否已初始化
    bool m_recording = false;                           // 是否正在录制
    std::string m_bestEncoder = "libx264";             // 最优编码器类型
    std::string m_bestEncoderFriendly = "CPU (x264)";  // 最优编码器友好显示名
    std::string m_ffmpegPath;                          // ffmpeg.exe 全路径

    WASAPILoopback m_loopback;                         // 原生系统声音采集器
    HANDLE m_hAudioPipe = INVALID_HANDLE_VALUE;        // 音频命名管道句柄
    bool m_recordSysAudio = false;                     // 是否启用了系统音频
    bool m_hasMicInput = false;                        // 是否启用了麦克风
    std::string m_finalOutputPath;                     // 最终 MP4 输出路径

    PROCESS_INFORMATION m_pi = {};                     // FFmpeg 进程信息
    HANDLE m_hStdInWrite = nullptr;                    // FFmpeg stdin 写入句柄
};
