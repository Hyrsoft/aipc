# RTSP 模块

`media_distribution/rtsp` 负责把 Producer 输出的 H.264/H.265 编码流推送到板端 RTSP 服务。

## 当前组件

```text
rtsp/
├── rk_rtsp.h/cpp        # 对 Luckfox/Rockchip RTSP 库的轻量封装
├── rtsp_service.h/cpp   # StreamManager 管理的 RTSP 服务
└── CMakeLists.txt
```

## 数据流

```text
EncodedStreamPtr
      |
      v
RtspService::StreamConsumer
      |
      v
RtspServer::PushFrame
      |
      v
rtsp://<device_ip>:554/live/0
```

`RtspService` 由 `StreamManager` 创建，并作为 `MediaManager` 的消费者注册。服务只有在 `Start()` 后才会真正向 RTSP server 推帧。

## HTTP API

- `GET /api/rtsp/status`：查看 RTSP 状态、URL 和发送统计。
- `POST /api/rtsp/start`：开始推送 RTSP 帧。
- `POST /api/rtsp/stop`：停止推送 RTSP 帧。

## 默认配置

入口在 `src/main.cpp` 中设置：

- 端口：`554`
- 路径：`/live/0`
- 默认创建服务但不自动启动，除非启动参数包含 `--rtsp`。

默认 URL：

```text
rtsp://<device_ip>:554/live/0
```
