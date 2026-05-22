# File 模块

`media_distribution/file` 负责把 Producer 输出的 H.264/H.265 编码流封装为 MP4 文件。当前文件模块只实现视频录制，不包含旧文档里提到的 `FileThread`、JPEG 拍照或 `rkvideo_register_stream_consumer` 接口。

## 当前组件

```text
file/
├── file_saver.h/cpp    # Mp4Recorder，使用 FFmpeg 写 MP4
├── file_service.h/cpp  # FileService，提供录制控制和流消费者入口
└── CMakeLists.txt
```

## 数据流

```text
EncodedStreamPtr
      |
      v
FileService::StreamConsumer
      |
      v
FileService::OnEncodedStream
      |
      v
Mp4Recorder::WriteFrame
      |
      v
MP4 file
```

`FileService` 由 `StreamManager` 创建，作为 `MediaManager` 的 `Queued` 类型消费者注册。这样文件写入不会阻塞主 IO 线程或网络发送路径。

## 录制控制

HTTP API：

- `GET /api/record/status`：查看录制状态、输出目录和统计信息。
- `POST /api/record/start`：开始录制。
- `POST /api/record/stop`：停止录制。

C++ 入口：

```cpp
FileServiceConfig config;
config.mp4Config.outputDir = "/root/record";

FileService service(config);
service.Start();
service.StartRecording();

// Producer 输出帧时调用：
FileService::StreamConsumer(stream, &service);

service.StopRecording();
service.Stop();
```

## 构建依赖

该模块从 `BUILDROOT_SYSROOT` 查找 FFmpeg 头文件和库：

- `libavformat`
- `libavcodec`
- `libswresample`
- `libavutil`
- `libswscale`

如果 sysroot 中缺少这些依赖，CMake 会在配置阶段直接失败。
