/**
 * 智眸录屏 - 前端交互控制逻辑 (main.js)
 * 负责 UI 事件监听、DOM 状态更新、计时器、与 C++ 后端 WebView2 IPC 通信
 */

// ==========================================
// 全局录制参数与状态
// ==========================================
let cropX = 0;                  // 录制起点 X
let cropY = 0;                  // 录制起点 Y
let cropW = 0;                  // 录制宽度
let cropH = 0;                  // 录制高度
let isRecording = false;        // 当前是否正在录制
let recordStartTime = null;     // 录制开始时间戳
let timerInterval = null;       // 计时器 Interval 定时器句柄
let defaultOutputFolder = '';   // 默认保存路径（系统 Videos 文件夹）
let lastRecordedFile = '';      // 最近录制的文件名

// DOM 元素引用 - 主面板布局
const gpuBadge = document.getElementById('gpu-badge');
const coordsText = document.getElementById('coords-text');
const selectAreaBtn = document.getElementById('select-area-btn');
const monitorSelect = document.getElementById('monitor-select');
let monitorsList = [];
const sysAudioToggle = document.getElementById('sys-audio-toggle');
const micAudioToggle = document.getElementById('mic-audio-toggle');
const micSelectContainer = document.getElementById('mic-select-container');
const micSelect = document.getElementById('mic-select');
const outputPathInput = document.getElementById('output-path');
const openFolderBtn = document.getElementById('open-folder-btn');
const previewText = document.getElementById('preview-text');
const recordBtn = document.getElementById('record-btn');
const recordBtnText = document.getElementById('record-btn-text');

// DOM 元素引用 - 悬浮控制条布局
const compactToolbar = document.getElementById('compact-toolbar');
const dragHandle = document.getElementById('drag-handle');
const compactTimer = document.getElementById('compact-timer');
const compactRes = document.getElementById('compact-res');
const compactRecordBtn = document.getElementById('compact-record-btn');
const compactCancelBtn = document.getElementById('compact-cancel-btn');

// ==========================================
// 页面初始化与 WebView2 事件绑定
// ==========================================
document.addEventListener('DOMContentLoaded', () => {
    if (window.chrome && window.chrome.webview) {
        // 绑定来自 C++ 宿主程序发送的消息监听
        window.chrome.webview.addEventListener('message', handleBackendMessage);
        // 向 C++ 发送初始化与加载状态请求
        sendMessageToBackend({ action: 'init' });
        sendMessageToBackend({ action: 'get_init_status' });
    } else {
        // 纯浏览器预览模式（非 C++ 环境）
        gpuBadge.textContent = "CPU (x264) - 浏览器预览模式";
        outputPathInput.value = "C:\\Users\\MockUser\\Videos";
    }
});

/**
 * @brief 向 C++ 后端发送 IPC JSON 消息
 * @param {Object} data 待发送的 JS 消息对象
 */
function sendMessageToBackend(data) {
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage(JSON.stringify(data));
    }
}

/**
 * @brief 接收并处理来自 C++ 后端的 JSON 消息
 * @param {MessageEvent} event 包含 C++ 回传数据的事件
 */
function handleBackendMessage(event) {
    let data = {};
    try {
        data = typeof event.data === 'string' ? JSON.parse(event.data) : event.data;
    } catch (e) {
        console.error("解析来自 C++ 的消息失败", e);
        return;
    }

    switch (data.action) {
        case 'init_status':
            // 资源与显卡探测就绪通知
            if (data.status === 'ready') {
                const titleText = document.getElementById('titlebar-text');
                if (titleText) titleText.textContent = "智眸录屏 - 资源加载完成";
                if (gpuBadge) {
                    gpuBadge.textContent = `✓ 资源加载完成 (${data.encoderFriendly})`;
                    gpuBadge.className = "badge ready";
                }
                setTimeout(() => {
                    if (titleText) titleText.textContent = "智眸录屏 - 极速 GPU 录制";
                }, 3000);
            }
            break;

        case 'init_response':
            // 初始化硬件与设备下拉框数据
            gpuBadge.textContent = "编码加速: " + data.encoder;
            defaultOutputFolder = data.defaultFolder;
            outputPathInput.value = defaultOutputFolder;

            // 加载显示器列表
            monitorsList = data.monitors || [];
            while (monitorSelect.options.length > 2) {
                monitorSelect.remove(2);
            }
            monitorsList.forEach((mon, index) => {
                const opt = document.createElement('option');
                opt.value = index;
                opt.textContent = `🖥️ ${mon.name}`;
                monitorSelect.appendChild(opt);
            });

            // 加载麦克风设备列表
            micSelect.innerHTML = '';
            if (data.mics && data.mics.length > 0) {
                data.mics.forEach(mic => {
                    const opt = document.createElement('option');
                    opt.value = mic.name;
                    opt.textContent = mic.name;
                    micSelect.appendChild(opt);
                });
            } else {
                const opt = document.createElement('option');
                opt.value = '';
                opt.textContent = "未检测到麦克风";
                micSelect.appendChild(opt);
                micAudioToggle.checked = false;
                micAudioToggle.disabled = true;
            }
            break;

        case 'select_area_response':
            // 处理框选覆盖层返回的结果
            if (data.cancelled) {
                cropX = cropY = cropW = cropH = 0;
                coordsText.textContent = "未选择区域（默认全屏）";
                coordsText.classList.remove('active');
                document.body.classList.remove('compact-mode');
                monitorSelect.value = 'custom';
            } else {
                cropX = data.x;
                cropY = data.y;
                cropW = data.w;
                cropH = data.h;
                coordsText.innerHTML = `区域: <strong>${cropW}x${cropH}</strong> (偏移 X:${cropX}, Y:${cropY})`;
                coordsText.classList.add('active');
                monitorSelect.value = 'custom';

                // 进入录制悬浮条紧凑模式
                compactRes.textContent = `${cropW}x${cropH}`;
                compactTimer.textContent = "准备就绪";
                document.body.classList.remove('recording');
                document.body.classList.add('compact-mode');
            }
            break;

        case 'crop_updated':
            // 响应 C++ RecordBorder 手柄拖拽/平移更新坐标
            cropX = data.x;
            cropY = data.y;
            cropW = data.w;
            cropH = data.h;
            coordsText.innerHTML = `区域: <strong>${cropW}x${cropH}</strong> (偏移 X:${cropX}, Y:${cropY})`;
            coordsText.classList.add('active');
            compactRes.textContent = `${cropW}x${cropH}`;
            break;

        case 'start_recording_response':
            // 录制启动结果反馈
            if (data.success) {
                isRecording = true;
                lastRecordedFile = data.filename;
                
                recordBtn.classList.add('recording');
                recordBtnText.textContent = "停止录制";
                document.body.classList.add('recording');
                
                startTimer();
            } else {
                alert("录制启动失败: " + data.error);
                previewText.textContent = "准备就绪";
                document.body.classList.remove('recording');
            }
            break;

        case 'stop_recording_response':
            // 录制停止结果反馈
            isRecording = false;
            
            recordBtn.classList.remove('recording');
            recordBtnText.textContent = "开始录制";
            document.body.classList.remove('recording');
            document.body.classList.remove('compact-mode');
            
            stopTimer();

            // 绘制录制完成与打开文件超链接
            const finalFilePath = defaultOutputFolder + "\\" + lastRecordedFile;
            previewText.innerHTML = `录制完成！已保存为 <a href="#" id="play-video-link" style="color:#00f0f0; text-decoration:underline;">${lastRecordedFile}</a>`;
            
            document.getElementById('play-video-link').addEventListener('click', (e) => {
                e.preventDefault();
                sendMessageToBackend({ action: 'open_file', file: finalFilePath });
            });
            break;
    }
}

// ==========================================
// 界面交互控件事件绑定
// ==========================================

// 麦克风开关与设备下拉框显示联动
micAudioToggle.addEventListener('change', () => {
    if (micAudioToggle.checked) {
        micSelectContainer.classList.remove('hidden');
    } else {
        micSelectContainer.classList.add('hidden');
    }
});

// 点击“框选区域”按钮
selectAreaBtn.addEventListener('click', () => {
    sendMessageToBackend({ action: 'select_area' });
});

// 点击“打开保存目录”按钮
openFolderBtn.addEventListener('click', () => {
    sendMessageToBackend({ action: 'open_folder', folder: outputPathInput.value });
});

// 显示器/区域选择下拉框切换
monitorSelect.addEventListener('change', () => {
    const val = monitorSelect.value;
    if (val === 'custom') {
        cropX = cropY = cropW = cropH = 0;
        coordsText.textContent = "未选择区域（默认全屏）";
        coordsText.classList.remove('active');
    } else if (val === 'all') {
        if (monitorsList.length > 0) {
            let minX = Math.min(...monitorsList.map(m => m.x));
            let minY = Math.min(...monitorsList.map(m => m.y));
            let maxX = Math.max(...monitorsList.map(m => m.x + m.w));
            let maxY = Math.max(...monitorsList.map(m => m.y + m.h));
            cropX = minX;
            cropY = minY;
            cropW = maxX - minX;
            cropH = maxY - minY;
            coordsText.innerHTML = `区域: <strong>全屏幕合并 (${cropW}x${cropH})</strong>`;
            coordsText.classList.add('active');
        } else {
            cropX = cropY = cropW = cropH = 0;
            coordsText.textContent = "未选择区域（默认全屏）";
            coordsText.classList.remove('active');
        }
    } else {
        const index = parseInt(val, 10);
        if (index >= 0 && index < monitorsList.length) {
            const mon = monitorsList[index];
            cropX = mon.x;
            cropY = mon.y;
            cropW = mon.w;
            cropH = mon.h;
            coordsText.innerHTML = `区域: <strong>显示器 ${index + 1} (${cropW}x${cropH})</strong>`;
            coordsText.classList.add('active');
        }
    }
});

// 点击“开始录制 / 停止录制”主按钮
recordBtn.addEventListener('click', () => {
    if (!isRecording) {
        const recordMic = micAudioToggle.checked && micSelect.value !== '';
        const micDevice = recordMic ? micSelect.value : '';
        const recordSysAudio = sysAudioToggle.checked;
        const outputFolder = outputPathInput.value;

        // 若未指定特殊选区，默认使用第一主显示器尺寸
        if (cropW === 0 || cropH === 0) {
            if (monitorsList.length > 0) {
                const mon = monitorsList[0];
                cropX = mon.x;
                cropY = mon.y;
                cropW = mon.w;
                cropH = mon.h;
            }
        }

        previewText.textContent = "正在初始化录屏...";
        
        sendMessageToBackend({
            action: 'start_recording',
            x: cropX,
            y: cropY,
            w: cropW,
            h: cropH,
            recordMic: recordMic,
            micDevice: micDevice,
            recordSysAudio: recordSysAudio,
            outputFolder: outputFolder
        });
    } else {
        previewText.textContent = "正在保存录屏...";
        sendMessageToBackend({ action: 'stop_recording' });
    }
});

// 悬浮手柄窗口按住拖拽
dragHandle.addEventListener('mousedown', () => {
    sendMessageToBackend({ action: 'start_drag' });
});

// 点击悬浮控制条中的取消选区按钮
compactCancelBtn.addEventListener('click', () => {
    sendMessageToBackend({ action: 'cancel_selection' });
    document.body.classList.remove('compact-mode');
    document.body.classList.remove('recording');
    cropX = cropY = cropW = cropH = 0;
    coordsText.textContent = "未选择区域（默认全屏）";
    coordsText.classList.remove('active');
});

// 点击悬浮控制条中的录制/停止按钮，委托至主按钮
compactRecordBtn.addEventListener('click', () => {
    recordBtn.click();
});

// ==========================================
// 录制计时器工具 (00:00:00 格式化)
// ==========================================
function startTimer() {
    recordStartTime = Date.now();
    updateTimerText();
    timerInterval = setInterval(updateTimerText, 1000);
}

function updateTimerText() {
    const elapsedMs = Date.now() - recordStartTime;
    const totalSecs = Math.floor(elapsedMs / 1000);
    
    const hrs = Math.floor(totalSecs / 3600).toString().padStart(2, '0');
    const mins = Math.floor((totalSecs % 3600) / 60).toString().padStart(2, '0');
    const secs = (totalSecs % 60).toString().padStart(2, '0');

    let sizeStr = "";
    if (cropW > 0 && cropH > 0) {
        sizeStr = ` [${cropW}x${cropH}]`;
    } else {
        sizeStr = " [全屏]";
    }

    const timeFormatted = `${hrs}:${mins}:${secs}`;
    previewText.innerHTML = `<span style="color:#ef4444; font-weight:600; margin-right:8px;">● REC</span> 正在录制: <strong style="font-family:monospace; font-size:14px;">${timeFormatted}</strong>${sizeStr}`;
    compactTimer.textContent = timeFormatted;
}

function stopTimer() {
    if (timerInterval) {
        clearInterval(timerInterval);
        timerInterval = null;
    }
    compactTimer.textContent = "准备就绪";
}

// ==========================================
// 自定义无边框窗口标题栏控件绑定
// ==========================================
const winMinBtn = document.getElementById('win-min-btn');
const winCloseBtn = document.getElementById('win-close-btn');
const titlebarDragArea = document.getElementById('titlebar-drag-area');

if (winMinBtn) {
    winMinBtn.addEventListener('click', () => {
        sendMessageToBackend({ action: 'minimize' });
    });
}

if (winCloseBtn) {
    winCloseBtn.addEventListener('click', () => {
        sendMessageToBackend({ action: 'close' });
    });
}

if (titlebarDragArea) {
    titlebarDragArea.addEventListener('mousedown', () => {
        sendMessageToBackend({ action: 'start_drag' });
    });
}

function logToBackend(msg) {
    sendMessageToBackend({ action: 'log', message: msg });
}

// ==========================================
// 动态 DOM 容器尺寸监听 (自动调适 Win32 窗口高度)
// ==========================================
let lastSentHeight = 0;
let resizeTimeout = null;
const resizeObserver = new ResizeObserver(entries => {
    for (let entry of entries) {
        if (document.body.classList.contains('compact-mode')) continue;
        const height = Math.ceil(entry.target.scrollHeight) + 28;
        
        if (Math.abs(height - lastSentHeight) > 2) {
            if (resizeTimeout) {
                clearTimeout(resizeTimeout);
            }
            resizeTimeout = setTimeout(() => {
                lastSentHeight = height;
                sendMessageToBackend({ action: 'resize_window', h: height });
                resizeTimeout = null;
            }, 200);
        }
    }
});

const contentWrapper = document.getElementById('content-measure-wrapper');
if (contentWrapper) {
    resizeObserver.observe(contentWrapper);
}

// 供 Windows 托盘右键菜单调用的全局钩子
window.__startRecording = () => {
    if (!isRecording) recordBtn.click();
};
window.__stopRecording = () => {
    if (isRecording) recordBtn.click();
};
