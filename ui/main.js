// Global recording parameters
let cropX = 0;
let cropY = 0;
let cropW = 0;
let cropH = 0;
let isRecording = false;
let recordStartTime = null;
let timerInterval = null;
let defaultOutputFolder = '';
let lastRecordedFile = '';

// DOM Elements - Main Layout
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

// DOM Elements - Compact Layout
const compactToolbar = document.getElementById('compact-toolbar');
const dragHandle = document.getElementById('drag-handle');
const compactTimer = document.getElementById('compact-timer');
const compactRes = document.getElementById('compact-res');
const compactRecordBtn = document.getElementById('compact-record-btn');
const compactCancelBtn = document.getElementById('compact-cancel-btn');

// Initialize communication with C++
document.addEventListener('DOMContentLoaded', () => {
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.addEventListener('message', handleBackendMessage);
        sendMessageToBackend({ action: 'init' });
        sendMessageToBackend({ action: 'get_init_status' });
    } else {
        gpuBadge.textContent = "CPU (x264) - 浏览器模式";
        outputPathInput.value = "C:\\Users\\MockUser\\Videos";
    }
});

// Helper to send messages to C++ backend
function sendMessageToBackend(data) {
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage(JSON.stringify(data));
    }
}

// Handle messages from C++ backend
function handleBackendMessage(event) {
    let data = {};
    try {
        data = typeof event.data === 'string' ? JSON.parse(event.data) : event.data;
    } catch (e) {
        console.error("Failed to parse message from C++", e);
        return;
    }

    switch (data.action) {
        case 'init_status':
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
            gpuBadge.textContent = "编码加速: " + data.encoder;
            defaultOutputFolder = data.defaultFolder;
            outputPathInput.value = defaultOutputFolder;

            // Load monitors
            monitorsList = data.monitors || [];
            // Remove previous dynamic options (leave custom and all)
            while (monitorSelect.options.length > 2) {
                monitorSelect.remove(2);
            }
            monitorsList.forEach((mon, index) => {
                const opt = document.createElement('option');
                opt.value = index;
                opt.textContent = `🖥️ ${mon.name}`;
                monitorSelect.appendChild(opt);
            });

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

                // Enter compact floating toolbar layout
                compactRes.textContent = `${cropW}x${cropH}`;
                compactTimer.textContent = "准备就绪";
                document.body.classList.remove('recording');
                document.body.classList.add('compact-mode');
            }
            break;

        case 'crop_updated':
            cropX = data.x;
            cropY = data.y;
            cropW = data.w;
            cropH = data.h;
            coordsText.innerHTML = `区域: <strong>${cropW}x${cropH}</strong> (偏移 X:${cropX}, Y:${cropY})`;
            coordsText.classList.add('active');
            compactRes.textContent = `${cropW}x${cropH}`;
            break;

        case 'start_recording_response':
            if (data.success) {
                isRecording = true;
                lastRecordedFile = data.filename;
                
                recordBtn.classList.add('recording');
                recordBtnText.textContent = "停止录制";
                document.body.classList.add('recording'); // Toggles compact pulse indicators
                
                startTimer();
            } else {
                alert("录制启动失败: " + data.error);
                previewText.textContent = "准备就绪";
                document.body.classList.remove('recording');
            }
            break;

        case 'stop_recording_response':
            isRecording = false;
            
            recordBtn.classList.remove('recording');
            recordBtnText.textContent = "开始录制";
            document.body.classList.remove('recording');
            document.body.classList.remove('compact-mode'); // Restores normal app layout
            
            stopTimer();

            // Display completion status in normal panel
            const finalFilePath = defaultOutputFolder + "\\" + lastRecordedFile;
            previewText.innerHTML = `录制完成！已保存为 <a href="#" id="play-video-link" style="color:#00f0f0; text-decoration:underline;">${lastRecordedFile}</a>`;
            
            document.getElementById('play-video-link').addEventListener('click', (e) => {
                e.preventDefault();
                sendMessageToBackend({ action: 'open_file', file: finalFilePath });
            });
            break;
    }
}

// Microphone toggle layout visibility
micAudioToggle.addEventListener('change', () => {
    if (micAudioToggle.checked) {
        micSelectContainer.classList.remove('hidden');
    } else {
        micSelectContainer.classList.add('hidden');
    }
});

// Area selector trigger
selectAreaBtn.addEventListener('click', () => {
    sendMessageToBackend({ action: 'select_area' });
});

// Open storage directory
openFolderBtn.addEventListener('click', () => {
    sendMessageToBackend({ action: 'open_folder', folder: outputPathInput.value });
});

// Monitor selection logic
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

// Record control main button logic
recordBtn.addEventListener('click', () => {
    if (!isRecording) {
        const recordMic = micAudioToggle.checked && micSelect.value !== '';
        const micDevice = recordMic ? micSelect.value : '';
        const recordSysAudio = sysAudioToggle.checked;
        const outputFolder = outputPathInput.value;

        // Default to Primary Monitor if no area is specified
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

// Drag handle for custom borderless window drag
dragHandle.addEventListener('mousedown', () => {
    sendMessageToBackend({ action: 'start_drag' });
});

// Compact mode cancel/back button
compactCancelBtn.addEventListener('click', () => {
    sendMessageToBackend({ action: 'cancel_selection' });
    document.body.classList.remove('compact-mode');
    document.body.classList.remove('recording');
    cropX = cropY = cropW = cropH = 0;
    coordsText.textContent = "未选择区域（默认全屏）";
    coordsText.classList.remove('active');
});

// Compact Record/Stop delegates directly to the validated main button
compactRecordBtn.addEventListener('click', () => {
    recordBtn.click();
});

// Timer formatting helper (00:00:00)
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

// Custom Titlebar controls
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

// Dynamic auto-resizing observer for normal window height
let lastSentHeight = 0;
let resizeTimeout = null;
const resizeObserver = new ResizeObserver(entries => {
    for (let entry of entries) {
        if (document.body.classList.contains('compact-mode')) continue;
        // Query the scrollHeight of #content-measure-wrapper and add 28px for .app-container's padding-bottom
        const height = Math.ceil(entry.target.scrollHeight) + 28;
        
        logToBackend(`JS: ResizeObserver triggered. contentHeight=${height - 28}, totalHeight=${height}, lastSentHeight=${lastSentHeight}, bodyHeight=${document.body.scrollHeight}`);
        
        // Only trigger if height changes by more than 2 pixels
        if (Math.abs(height - lastSentHeight) > 2) {
            if (resizeTimeout) {
                clearTimeout(resizeTimeout);
                logToBackend(`JS: Cleared pending resize timeout`);
            }
            logToBackend(`JS: Scheduling resize to height=${height}`);
            resizeTimeout = setTimeout(() => {
                lastSentHeight = height;
                logToBackend(`JS: Timeout fired! Sending resize_window h=${height}`);
                sendMessageToBackend({ action: 'resize_window', h: height });
                resizeTimeout = null;
            }, 200);
        }
    }
});

const contentWrapper = document.getElementById('content-measure-wrapper');
if (contentWrapper) {
    // Start observing size updates
    resizeObserver.observe(contentWrapper);
}

// Expose hooks for system tray menu (called via C++ ExecuteScript)
window.__startRecording = () => {
    if (!isRecording) recordBtn.click();
};
window.__stopRecording = () => {
    if (isRecording) recordBtn.click();
};
