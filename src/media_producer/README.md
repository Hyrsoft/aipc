# Media Producer 模块

`media_producer` 负责视频生产侧：创建采集/处理模式、管理模式生命周期，并把编码后的 H.264 流交给分发模块。

当前实现只有两种 Producer：

- `SimpleIPCProducer`：纯监控模式，使用 RKMPI 建立 `VI -> VPSS -> VENC` 硬件绑定。
- `VisionGProducer`：Python 驱动 AI 模式，Python 负责摄像头、推理、绘制和帧循环，C++ 负责嵌入式 Python 生命周期、VENC 编码和流媒体分发。

旧的 `yolov5/`、`retinaface/`、`rknn/`、`rkvideo/` 子目录已不属于当前架构；模型逻辑现在放在 Python 工程中。

## 目录结构

```text
media_producer/
├── i_media_producer.h          # Producer 公共接口和模式枚举
├── media_manager.h/cpp         # 生命周期管理、冷切换、消费者重注册
├── simple_ipc/
│   ├── simple_ipc_config.h     # SimpleIPC 分辨率/帧率预设
│   ├── simple_ipc_producer.h
│   ├── simple_ipc_producer.cpp
│   └── mpi_config.h            # SimpleIPC 的 RKMPI 配置辅助函数
└── visiong/
    ├── visiong_producer.h
    └── visiong_producer.cpp
```

## 数据流

### SimpleIPC

```text
VI -> VPSS -> VENC -> EncodedStreamPtr -> RTSP / WebSocket Preview / File / WebRTC
```

特点：

- 使用硬件绑定，CPU 不参与帧处理。
- 分辨率通过 `SimpleIPCConfig` 和 `/api/pipeline/resolution` 切换。
- 切换分辨率需要停止并重新初始化 SimpleIPC 管线。

### VisionG

```text
Python project
  visiong.Camera -> model inference / drawing -> aipc.submit_frame(frame)
                                                      |
                                                      v
                                             C++ VENC encode
                                                      |
                                                      v
                               RTSP / WebSocket Preview / File / WebRTC
```

Python 工程契约：

- `init()`：可选，用于创建 `visiong.Camera`、加载模型等。
- `run()`：必须存在，通常循环检查 `aipc.is_running()`。
- `cleanup()`：可选，用于释放摄像头、模型等资源。
- `aipc.submit_frame(frame)`：把处理后的 `visiong.ImageBuffer` 交给 C++ 编码。

不要在 AIPC Python 工程里使用 VisionG 的 `DisplayUDP`、`DisplayHTTP`、`DisplayRTSP` 作为主要输出；AIPC 的输出由 C++ 分发模块统一处理。

## MediaManager

`MediaManager` 是 Producer 的统一生命周期控制器。

主要职责：

- 持有当前 `IMediaProducer` 实例。
- 根据模式创建 `SimpleIPCProducer` 或 `VisionGProducer`。
- 执行冷切换：停止旧 Producer、释放资源、创建新 Producer、重新注册消费者、按需启动。
- 保存消费者注册信息，切换模式后自动恢复 RTSP、WebSocket Preview、File、WebRTC 的连接。

典型调用：

```cpp
auto& mgr = media::MediaManager::Instance();

media::ProducerConfig config;
config.framerate = 30;
config.bitrate_kbps = 10 * 1024;

mgr.Init(media::ProducerMode::SimpleIPC, config);
mgr.RegisterStreamConsumer("rtsp", rtsp_callback);
mgr.RegisterStreamConsumer("ws_preview", ws_callback);
mgr.Start();

mgr.SwitchMode(media::ProducerMode::VisionG);
mgr.SwitchMode(media::ProducerMode::SimpleIPC);

mgr.Stop();
mgr.Deinit();
```

## HTTP 相关接口

- `GET /api/producer/status`：查看当前 Producer 模式。
- `POST /api/producer/switch`：切换 `simple_ipc` / `visiong`。
- `GET /api/ai/status`：前端兼容接口，VisionG 模式视为 AI 已启用。
- `POST /api/ai/switch`：前端兼容接口，`visiong` 切到 VisionG，其他值切回 SimpleIPC。
- `GET /api/pipeline/status`：前端兼容接口，SimpleIPC 显示为 `parallel`，VisionG 显示为 `serial`。
- `POST /api/pipeline/resolution`：只在 SimpleIPC 模式下切换分辨率。
- `POST /api/python/deploy`：在 VisionG 模式下部署 Python 工程。

## 注意事项

- VisionG 的摄像头分辨率由 Python 工程里的 `visiong.Camera(...)` 决定，C++ 不提供 VisionG 分辨率切换接口。
- 模式切换是冷切换，会短暂停流。
- Python 运行时按进程生命周期初始化，避免每次切换都销毁解释器。
