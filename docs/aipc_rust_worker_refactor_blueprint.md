# AIPC Rust/C++ Worker 重构蓝图

## 1. 目的与范围

本蓝图描述 AIPC 从单体 C++ 应用演进为“Rust 业务主进程 + 最小化原生 worker”的目标架构与实施顺序。

本阶段只定义职责、数据语义、阶段边界和验收标准，不决定具体的 IPC、序列化、网络库、Rust crate 或构建实现。现有 C++ 应用继续作为运行参考；本蓝图本身不要求修改代码、构建、依赖或部署脚本。

目标是将必须直接使用硬件 SDK 的媒体与 AI 能力隔离到独立进程，同时把网络服务、Web UI、配置和复杂业务逻辑收敛到 Rust 主进程。这样既保留 RKMPI/RKNN 等原生调用的可控边界，也让高层业务具备更清晰的生命周期、恢复和扩展能力。

## 2. 当前基线

当前实现以 C++ `aipc` 进程为入口：

- `SimpleIPCProducer` 已实现基于 RKMPI 的 VI → VPSS → VENC 硬件绑定视频链路；
- 同一进程还承担 HTTP API、Web UI、RTSP、WebRTC、WebSocket 预览、录制和流分发；
- VisionG/Python 模式将 AI 推理、脚本运行和媒体生命周期耦合在当前主程序中；
- 目标板为 Luckfox Pico / RV1106 系列，运行 uClibc Linux，并依赖板端 ISP、RKMPI 与后续 RKNN 能力。

重构后不应让 Rust 直接承担底层 ISP、RKMPI、音视频编解码或 RKNN 资源所有权；也不应继续让 C++ worker 承担网络协议、浏览器控制或业务编排。

## 3. 目标架构与职责

```text
浏览器 / RTSP 客户端 / WebRTC 客户端 / 录制任务
                         │
                         ▼
                 Rust aipc-daemon
  Web UI · 控制 API · 配置状态 · worker 监督 · 流分发 · 录制 · 指标
                         │
          编码音视频流、控制、事件、状态
                         │
                         ▼
                 C++ media worker
      ISP · VI · VPSS · VENC · 音频采集 · 音频编码

                 C++ AI worker（后续）
        RKNN · 低分辨率旁路帧 · 推理 · AI 结果/指标
```

### Rust `aipc-daemon`

Rust daemon 是未来的业务主进程，负责：

- 启动、停止、监督、重启和回滚 media worker 与后续 AI worker；
- 获取 worker 输出的编码视频和音频，并维护流生命周期、时间戳、关键帧、generation 与健康状态；
- 承载 RTSP、WebRTC、WebSocket 预览、录制及其他网络分发业务；
- 提供 Web UI 静态资源、控制 API、状态查询、配置修改、日志与指标聚合；
- 为每类下游消费者提供独立的有界缓冲和丢帧边界，避免慢客户端、录制任务或单个协议服务阻塞主码流；
- 保存 desired、pending 与 last-good 配置状态，并在失败时协调恢复。

### C++ media worker

media worker 只拥有底层音视频资源和与其直接相关的状态机：

- ISP、VI、VPSS、VENC 的初始化、绑定、停止、解绑和释放；
- 视频采集、处理和编码；
- 后续音频采集与编码；
- 已编码音视频流、生命周期事件、错误与基础指标输出；
- 收到停止请求后的有序清理和进程退出。

worker 不包含 HTTP、Web UI、RTSP、WebRTC、WebSocket、录制、业务配置持久化、网络客户端管理、VisionG/Python 或 AI 编排。

### C++ AI worker（后续）

AI worker 仅封装 RKNN、必要的底层图像处理和推理执行：

- 消费媒体管线提供的低分辨率旁路帧；
- 产出推理结果、告警、错误和指标；
- 不拥有主码流的生命周期，不直接向网络客户端发送视频；
- 推理故障、重启或过载不得阻塞或降级主音视频流。

## 4. 分阶段路线

### Phase 1：C++ 基础音视频 worker

先从现有 SimpleIPC 视频路径提炼独立 media worker，形成与网络业务无关的基础音视频闭环。

- 复用并整理 VI → VPSS → VENC 的硬件链路，使分辨率、帧率、码率和硬件通道不再只能依赖编译期常量；
- 实现独立的视频 H264 输出、音频采集与编码输出；
- 实现启动进度、流就绪、运行指标、警告、致命错误和退出状态；
- 为信号退出、初始化失败、绑定失败、无帧和编码失败提供一致的清理路径；
- 在板端验证独立 worker 可以生成可解析、可播放的基础音视频产物。

第一阶段的重点是媒体硬件资源所有权和稳定性，不包含网络分发、Web UI 或 AI 功能。

### Phase 2：Rust 主进程与媒体业务闭环

Rust daemon 接管 worker 的进程管理与上层业务。

- 启动 media worker，传入完整配置，并接收音视频流、状态事件和诊断信息；
- 检测启动、首次可用视频、关键帧、音频可用、无帧、无关键帧、异常退出和停止完成；
- 实现配置变更的冷重启：停止旧 worker、启动新 generation、确认新流可用后再提交状态；
- 实现失败处理和回滚：新配置启动失败或流超时后恢复 last-good 配置；
- 将编码流分发到 WebSocket 预览、RTSP、WebRTC、录制及后续消费者；
- 提供 Web UI、控制 API、状态查询、配置修改、日志和指标接口。

Phase 2 完成后，Rust daemon 成为运行时唯一的业务入口；C++ media worker 仍只负责底层媒体能力。

### Phase 3：AI 进程隔离

在主媒体与网络闭环稳定后，再新增独立 AI worker。

- 由 Rust daemon 创建、停止和监督 AI worker；
- 从 media worker 的低分辨率旁路获取 AI 所需输入，不抢占主编码链路；
- 将推理结果返回 Rust daemon，由 daemon 负责业务规则、Web UI 呈现、告警和对外 API；
- 验证 AI worker 的退出、崩溃、卡顿和重启不会影响主音视频流和已连接客户端。

## 5. 逻辑接口与数据语义

具体协议以后续技术方案确定；以下名称与语义用于保证各阶段接口可演进。

### 配置域

- `MediaConfig`：一次媒体运行所需的完整配置快照；
- `VideoConfig`：编码格式、分辨率、帧率、码率、关键帧与输出参数；
- `AudioConfig`：采集、编码、采样率、声道和输出参数；
- `ViConfig`、`VpssConfig`、`VencConfig`：底层视频通道与硬件资源配置；
- `NetworkConfig`：Rust 侧分发、录制和外部服务配置；
- `AiConfig`：AI worker 的模型、旁路输入和结果策略配置。

### 编码流语义

每个从 media worker 传出的编码包需要表达：

- 音频或视频类型、编码格式与负载；
- `sequence`、`pts`、可用时的 `dts`；
- 视频关键帧标志；
- `generation`，用于区分冷重启前后的流；
- 必要的格式变化信息，供 Rust 分发层安全地建立新消费者。

### Worker 事件语义

worker 与 daemon 间至少应可表达：

- `BootProgress`：初始化阶段与进度；
- `StreamReady`：媒体链路已产出可消费的流；
- `Warning`：可恢复的问题；
- `FatalError`：不可恢复的问题及原因；
- `StreamStalled`：音频或视频在预期时间内未继续产出；
- `Metrics`：帧率、码率、延迟、错误计数和资源相关指标；
- `Stopped`：完成资源释放后的退出确认。

## 6. 故障边界与运行原则

- Rust daemon 是唯一负责业务恢复决策的进程；worker 报告事实、完成本地清理并退出。
- media worker 失败时，daemon 必须隔离该失败、记录诊断，并按配置恢复策略重启或回滚。
- 下游网络消费者只能影响自身队列和连接状态，不能阻塞 media worker 或其他消费者。
- AI worker 被视为可选旁路服务：任何 AI 故障都不得导致主视频、主音频或网络服务整体退出。
- 每次冷重启均创建新的 generation；Rust 仅在新 generation 已就绪后将其作为当前可用流。

## 7. 阶段验收标准

| 阶段 | 验收结果 |
| --- | --- |
| Phase 1 | media worker 可在板端独立启动、停止并清理硬件资源；可产出可解析的 H264 视频与基础音频编码产物；错误可观测。 |
| Phase 2 | daemon 可控制 worker 生命周期、接收并分发编码流、处理超时与异常退出；Web UI 与 API 可观察和控制媒体状态；配置冷重启可产生新 generation。 |
| Phase 3 | AI worker 能独立运行、重启和报告结果；AI 异常不会中断主音视频流、网络分发或既有客户端。 |

## 8. 非目标与后续决策

本蓝图不在当前阶段决定以下事项：

- 进程间通信、消息序列化、流传输与配置持久化的具体实现；
- Rust 依赖、C++ 第三方库、构建编排与交叉编译细节；
- RTSP、WebRTC、WebSocket 与录制功能的具体迁移次序和网络实现；
- 音频硬件通道、音频编码格式及板端部署依赖；
- AI 模型、Lua 或其他编排机制的选择。

这些决策应在各实施阶段开始前，结合目标板可用 SDK、性能、内存占用、部署方式和回归测试结果另行形成技术方案。
