# AIPC Edge AI Camera

基于瑞芯微 RV1106 平台的高性能边缘智能网络摄像头设计，支持 RTSP 推流、WebRTC、mp4 离线录制、WebSocket 网页实时预览，通过 RKNN 部署轻量级视觉模型。

## About

AIPC 是面向 Luckfox Pico / RV1106 的边缘智能摄像头应用，采用 C++ 服务端 + Python AI 脚本协同架构，支持在设备端完成采集、推流、录制与视觉推理，并通过 Web 控制台进行管理和热部署。

## 核心功能

- 双模式采集与处理
	- SimpleIPC 模式：硬件链路直通，低开销稳定推流
	- VisionG 模式：Python 驱动摄像头与推理流程
- 多路媒体分发
	- RTSP 实时推流
	- WebRTC 低延迟预览
	- WebSocket 网页 H264 预览
	- MP4 本地离线录制
- AI 工程热更新
	- Web 端创建/编辑/部署 Python 工程
	- 运行时切换模型逻辑，无需整机重启
- 统一 HTTP API
	- 设备状态、模式切换、流服务控制、Python 工程管理

## 技术栈

- C++17 + CMake
- pybind11 嵌入 Python 3.11
- cpp-httplib（HTTP API）
- libdatachannel（WebRTC）
- spdlog（日志）
- Rockchip RKMPI（VI/VPSS/VENC）
- VisionG + RKNN（边缘推理）

## 快速开始

### 1. 编译

```bash
cmake --build build/Debug
```

### 2. 一键部署到板端

```bash
./assets/install_rsync.sh
```

### 3. 板端启动

```bash
ssh root@192.168.8.235
cd /root/aipc/bin
./start_app.sh --daemon
```

### 4. 访问控制台

- Web UI: http://192.168.8.235:8080
- 健康状态: http://192.168.8.235:8080/api/status

## 常用接口

- `POST /api/ai/switch`：切换 AI 模式（`visiong` / `none`）
- `GET /api/python/projects`：获取 Python 工程列表
- `POST /api/python/deploy`：部署指定 Python 工程
- `POST /api/rtsp/start`：启动 RTSP
- `POST /api/webrtc/start`：启动 WebRTC

## 目录参考

- `src/`：核心 C++ 服务与媒体管线
- `assets/`：部署脚本、模型与内置 Python 工程
- `www/`：前端控制台
- `docs/`：架构与调试文档

## License

本项目遵循仓库内各组件对应的开源许可证。

