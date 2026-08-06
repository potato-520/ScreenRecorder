#!/bin/bash

# Configuration paths for Windows MSVC Build Tools
CMAKE_EXE="/mnt/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
MSBUILD_EXE="/mnt/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe"

# Make sure we are in the project root directory
cd "$(dirname "$0")"

# Check if compilers exist
if [ ! -f "$CMAKE_EXE" ]; then
    echo "错误：未在预期路径找到 Windows CMake，请检查 Visual Studio 2022 Build Tools 安装情况。"
    echo "预期路径：$CMAKE_EXE"
    exit 1
fi

if [ ! -f "$MSBUILD_EXE" ]; then
    echo "错误：未在预期路径找到 MSBuild.exe，请检查 Visual Studio 2022 Build Tools 安装情况。"
    echo "预期路径：$MSBUILD_EXE"
    exit 1
fi

echo "============================================="
echo "  1. 正在使用 Windows CMake 生成项目结构...  "
echo "============================================="

# Run CMake configuration (outputs to log to prevent TTY hangs, then cat)
"$CMAKE_EXE" -B build -G "Visual Studio 17 2022" -A x64 > cmake_config.log 2>&1
CMAKE_STATUS=$?
cat cmake_config.log
rm -f cmake_config.log

if [ $CMAKE_STATUS -ne 0 ]; then
    echo "❌ CMake 生成项目失败！"
    exit 1
fi

echo "============================================="
echo "  2. 正在使用 MSBuild 编译 Release 版本...    "
echo "============================================="

# Run MSBuild via PowerShell (reliable after WSL restarts, no binfmt dependency)
powershell.exe -NoProfile -Command "
  Set-Location 'C:\myprog\record_video\build'
  \$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
  & \$msbuild 'ScreenRecorder.sln' /p:Configuration=Release /p:Platform=x64 /nologo
  exit \$LASTEXITCODE
" > msbuild_build.log 2>&1
BUILD_STATUS=$?
cat msbuild_build.log
rm -f msbuild_build.log

if [ $BUILD_STATUS -ne 0 ]; then
    echo "❌ MSBuild 编译失败！"
    exit 1
fi

# Ensure bin directory exists at the root and copy target executable there
mkdir -p bin
cp build/bin/Release/ScreenRecorder.exe bin/ 2>/dev/null
# Clean up old DLL and UI copies to verify pure single-EXE setup
rm -f bin/WebView2Loader.dll
rm -rf bin/ui

echo "============================================="
echo "🎉 编译成功！"
echo "可执行程序已输出至本地目录："
echo "Windows 路径: C:\\myprog\\record_video\\bin\\ScreenRecorder.exe"
echo "WSL 路径:     /mnt/c/myprog/record_video/bin/ScreenRecorder.exe"
echo "============================================="
