#pragma once

#include <windows.h>

/// 框选结果矩形参数
struct CropRect {
    int x = 0;           // 起点 X 坐标
    int y = 0;           // 起点 Y 坐标
    int w = 0;           // 矩形宽度
    int h = 0;           // 矩形高度
    bool cancelled = false; // 用户是否取消了框选
};

/**
 * @brief 全屏鼠标拖拽框选覆盖层窗口 (SelectionOverlay)
 * 
 * 弹出全屏暗色透明蒙版，允许用户使用鼠标左键拖拽绘制录像捕获区域。
 */
class SelectionOverlay {
public:
    /**
     * @brief 弹出全屏蒙版并阻塞等待用户完成框选或取消
     * @return 返回用户框选的矩形参数 CropRect
     */
    static CropRect Show(HINSTANCE hInstance);

private:
    /// Win32 蒙版窗口过程消息处理函数
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
};
