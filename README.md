# 智眸录屏 (ScreenRecorder)

<p align="center">
  <b>一款基于 C++17、WebView2 与 FFmpeg 的超轻量 GPU 硬件加速屏幕录制软件</b>
</p>

---

## 🌟 核心特性亮点

- 🚀 **GPU 硬件加速**：自动探测显卡类型（NVIDIA NVENC / Intel QuickSync / AMD AMF / Media Foundation），支持 1080P/4K 高帧率流畅录制，极低 CPU 与内存占用。
- 🎯 **全穿透交互式录屏框**：
  - **8 节点缩放**：支持拖拽四角与四边控制手柄自由调整录像分辨率。
  - **按住拖拽**：可随时拖动顶部标题栏平移捕获区域。
  - **100% 鼠标穿透**：录屏框中间透明区域支持鼠标直接穿透，不妨碍对桌面和底层软件的点击与操作。
- 🔊 **原生系统声卡与麦克风采集**：基于 WASAPI Loopback 双通道硬件回放捕获，自带静音防断流补帧机制，支持系统音与麦克风混音录制。
- 📦 **单文件绿色便携**：零依赖，资源与 FFmpeg 核心引擎打包于单一 EXE 中，自动释放运行，干净无残留。
- 🎨 **赛博朋克极客 UI**：基于 Microsoft WebView2 + HTML5/CSS3，提供现代化深色玻璃质感 UI 和紧凑型悬浮控制条。

---

## 🛠️ 技术栈与架构

- **核心语言**：C++17 (MSVC)
- **界面引擎**：Microsoft WebView2 (Chromium Webview)
- **视频录制引擎**：FFmpeg (GDIGRAB + GPU Codecs + fragmented MP4)
- **音频采集引擎**：Windows WASAPI Loopback (原生声卡捕获) + DirectShow
- **构建系统**：CMake + MSBuild

---

## 🚀 编译与构建说明

### 环境要求
- Windows 10 / 11 (X64)
- Visual Studio 2022 (或 Visual Studio Build Tools 2022)
- Windows SDK 10.0+
- CMake 3.20+

### 构建步骤

使用根目录的脚本一键编译：

```bash
# 在 WSL 或 Windows 命令行下运行构建脚本
bash build.sh
```

构建成功后，可执行文件将生成于 `bin/ScreenRecorder.exe`。

---

## 📖 使用指南

1. **选择录制区域**：
   - 点击 **“全屏”** 快速捕获整个显示器；
   - 或点击 **“框选区域”**，拖拽绘制捕获框。通过手柄缩放或拖动状态栏调整好位置后，直接点击 **“开始录制”**。
2. **声音设置**：
   - 勾选 **“系统声音”** 录制电脑播放的音频；
   - 勾选 **“麦克风”** 并选择下拉菜单中的麦克风设备进行语音录像。
3. **查看录像**：
   - 录制结束后，点击界面上的视频链接或 **“打开文件夹”** 按钮即可快速查看保存的 MP4 视频。

---

## 📜 许可协议

本项目采用 [Apache License 2.0](LICENSE) 开源许可。
