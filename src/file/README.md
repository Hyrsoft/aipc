# 文件保存模块 (File Saver)

文件保存模块实现视频离线录制（MP4）和拍照（JPEG）功能，作为编码流分发器的消费者之一。

## 设计原则

- **RAII 设计**：`Mp4Recorder` 和 `JpegCapturer` 使用构造函数初始化，析构函数清理，无需额外的 init/deinit
- **独立线程**：`FileThread` 作为独立的控制线程，便于后续通过 WebSocket/HTTP API 控制
- **解耦设计**：文件保存逻辑与流分发逻辑分离，便于维护和扩展

## 功能特性

### MP4 录制
- 使用 FFmpeg 封装 H.264/H.265 编码流为 MP4 文件
- 支持自动时间戳命名或自定义文件名
- 支持最大录制时长限制
- 支持最大文件大小限制
- 零拷贝接收 VENC 编码流

### JPEG 拍照
- 使用 RKMPI JPEG 编码器实现快照
- 使用 TDE 进行硬件加速缩放
- 支持自定义 JPEG 质量（1-100）
- 支持图片旋转（0°, 90°, 180°, 270°）

## 架构设计

```
┌─────────────────────────────────────────────────────┐
│                 StreamDispatcher                     │
│  (从 VENC 获取 H.264 编码流，分发给多个消费者)         │
└────────────────────────┬────────────────────────────┘
                         │
         ┌───────────────┼───────────────┐
         ▼               ▼               ▼
    ┌──────────┐   ┌───────────┐   ┌──────────┐
    │RtspThread│   │ FileThread│   │(WebRTC)  │
    │ RTSP推流 │   │ 文件保存  │   │  待实现  │
    └──────────┘   └─────┬─────┘   └──────────┘
                         │
              ┌──────────┴──────────┐
              ▼                     ▼
        ┌───────────┐        ┌────────────┐
        │Mp4Recorder│        │JpegCapturer│
        │ (RAII)    │        │  (RAII)    │
        └───────────┘        └────────────┘
```

## 使用方法

### 基本使用

```cpp
#include "file/thread_file.h"

// 配置
FileThreadConfig config;
config.mp4Config.outputDir = "/sdcard/videos";
config.mp4Config.filenamePrefix = "record";
config.jpegConfig.outputDir = "/sdcard/photos";
config.jpegConfig.viDevId = 0;
config.jpegConfig.viChnId = 0;

// 创建并启动文件线程
CreateFileThread(config);

// 注册到流分发器
rkvideo_register_stream_consumer(
    "file",
    &FileThread::StreamConsumer,
    GetFileThread(),
    5
);

GetFileThread()->Start();

// 录制控制
GetFileThread()->StartRecording();      // 开始录制
GetFileThread()->StopRecording();       // 停止录制

// 拍照
GetFileThread()->TakeSnapshot();        // 触发拍照

// 清理
DestroyFileThread();
```

### 直接使用类

```cpp
#include "file/file_saver.h"

// MP4 录制（RAII，构造即可用）
Mp4RecordConfig mp4_config;
mp4_config.outputDir = "/tmp";
Mp4Recorder recorder(mp4_config);

recorder.StartRecording("my_video");
// ... 调用 WriteFrame() 写入帧 ...
recorder.StopRecording();

// JPEG 拍照（RAII）
JpegCaptureConfig jpeg_config;
jpeg_config.viDevId = 0;
jpeg_config.viChnId = 0;
JpegCapturer capturer(jpeg_config);

// 在独立线程中运行处理循环
std::atomic<bool> running{true};
std::thread t([&]() { capturer.ProcessLoop(running); });

capturer.TakeSnapshot();  // 触发拍照

running = false;
t.join();
```

## 文件结构

```
src/file/
├── CMakeLists.txt    # CMake 配置
├── file_saver.h      # MP4Recorder / JpegCapturer 类定义
├── file_saver.cpp    # 实现
├── thread_file.h     # FileThread 线程类定义
├── thread_file.cpp   # 线程实现
└── README.md         # 本文档
```

## 控制接口（便于 WebSocket/HTTP 集成）

`FileThread` 提供以下控制接口：

| 方法 | 说明 |
|------|------|
| `StartRecording(filename)` | 开始录制 |
| `StopRecording()` | 停止录制 |
| `IsRecording()` | 检查录制状态 |
| `GetRecordStats()` | 获取录制统计 |
| `TakeSnapshot(filename)` | 触发拍照 |
| `GetLastPhotoPath()` | 获取最后照片路径 |
| `GetPhotoCount()` | 获取拍照计数 |

## 注意事项

1. **VI 通道配置**: JPEG 拍照需要 VI 通道 `u32Depth >= 1`
2. **VENC 通道**: JPEG 使用独立通道（默认 1），避免与 H.264 冲突
3. **线程安全**: 所有控制接口都是线程安全的

## 参考

- [main_ffmpeg.c](../../rockit-test/src/main_ffmpeg.c) - FFmpeg MP4 录制示例
- [main_jpeg.c](../../rockit-test/src/main_jpeg.c) - JPEG 拍照示例
