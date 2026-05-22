# WebRTC 模块

`media_distribution/webrtc` 负责把 Producer 输出的 H.264 编码流通过 libdatachannel 推给浏览器。

## 当前组件

```text
webrtc/
├── signaling.h/cpp        # WebSocket 信令客户端
├── webrtc.h/cpp           # PeerConnection、媒体轨道、H.264 RTP 打包
├── webrtc_service.h/cpp   # StreamManager 管理的 WebRTC 服务封装
└── CMakeLists.txt
```

## 数据流

```text
EncodedStreamPtr
      |
      v
WebRTCService::StreamConsumer
      |
      v
WebRTCSystem::SendVideoData
      |
      v
libdatachannel PeerConnection
```

`WebRTCService` 由 `StreamManager` 创建。服务启动后会初始化 `WebRTCSystem`，并按配置连接信令服务器；前端也可以通过 HTTP 信令接口完成 offer/answer/ICE 交换。

## HTTP API

- `GET /api/webrtc/status`：查看 WebRTC 服务状态。
- `POST /api/webrtc/start`：启动 WebRTC 服务。
- `POST /api/webrtc/stop`：停止 WebRTC 服务。
- `POST /api/webrtc/offer`：由设备创建 SDP Offer。
- `POST /api/webrtc/answer`：提交浏览器 SDP Answer。
- `POST /api/webrtc/ice`：提交浏览器 ICE candidate。
- `GET /api/webrtc/candidates`：获取设备本地 ICE candidates。

## 配置

入口在 `src/main.cpp` 中设置：

- `device_id` 默认为 `aipc_camera`。
- `SIGNALING_HOST` 环境变量用于生成默认信令地址 `ws://<host>:8000/`。
- 视频参数默认 `1920x1080 @ 30fps`。
- 默认创建服务但不自动启动，除非启动参数包含 `--webrtc`。

## 注意事项

- WebRTC 只消费已经编码好的 H.264 帧，不负责采集或 AI 推理。
- `WebRTCService::IsRunning()` 当前以内部 `valid_` 状态表示服务是否初始化成功。
- 如果只需要浏览器低延迟预览，WebSocket Preview 路径实现更简单；如果需要浏览器原生实时传输和 NAT/ICE 能力，使用 WebRTC。
