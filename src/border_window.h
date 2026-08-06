#pragma once

#include <windows.h>
#include <functional>

/**
 * @brief 交互式录屏边框窗口类 (RecordBorder)
 * 
 * 负责在屏幕上绘制捕获区域的外框线、8个拖拽手柄以及顶部分辨率指示牌。
 * 支持鼠标拖拽移动、四角/四边拉伸缩放，且框内区域保持 100% 鼠标点击穿透。
 */
class RecordBorder {
public:
    /// 录制区域发生变动（拖拽/缩放）时的回调函数类型
    using AreaChangedCallback = std::function<void(int x, int y, int w, int h)>;

    RecordBorder();
    ~RecordBorder();

    /**
     * @brief 创建并显示录制边框窗口
     * @param hInstance 应用程序句柄
     * @param x 捕获区域起点 X 坐标
     * @param y 捕获区域起点 Y 坐标
     * @param w 捕获区域宽度
     * @param h 捕获区域高度
     * @return 成功返回 true，失败返回 false
     */
    bool Create(HINSTANCE hInstance, int x, int y, int w, int h);

    /**
     * @brief 设置录制区域位置或大小发生改变时的回调函数
     */
    void SetOnAreaChanged(AreaChangedCallback cb) { m_onAreaChanged = cb; }

    /**
     * @brief 切换录制状态（修改边框颜色与手柄可见性）
     * @param recording true 为录制中（红色边框，锁定手柄），false 为空闲中（青色边框，显示手柄）
     */
    void SetRecording(bool recording);

    /**
     * @brief 获取当前最新的录制区域坐标与尺寸
     */
    void GetArea(int& x, int& y, int& w, int& h) const;

    /**
     * @brief 销毁边框窗口
     */
    void Destroy();

    /**
     * @brief 检查边框窗口当前是否激活
     */
    bool IsActive() const { return m_hWnd != NULL; }

private:
    /// Win32 窗口过程消息处理函数
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    
    /// 根据当前窗口实际位置更新内部录制坐标参数并触发回调
    void UpdateGeometryFromWindowRect();

private:
    HWND m_hWnd = NULL;                          // 边框窗口句柄
    bool m_recording = false;                   // 是否正在录制中
    int m_cropX = 0;                             // 实际录制区域起点 X
    int m_cropY = 0;                             // 实际录制区域起点 Y
    int m_cropW = 0;                             // 实际录制区域宽度
    int m_cropH = 0;                             // 实际录制区域高度
    AreaChangedCallback m_onAreaChanged = nullptr; // 几何变动回调
};
