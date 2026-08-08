# AI worker、Lua 项目管理与非阻塞 OSD 架构

状态：实施契约
目标分支：`feat/ai_worker_lua`
基线：`main` + WebRTC 提交 `4d03e6b`
目标平台：Luckfox Pico Ultra W / RV1106，Buildroot Linux，armv7 uClibc

## 1. 目标与边界

本改造为当前 Rust daemon + C++ media worker 架构增加独立 AI 进程、Lua
算法项目管理、动态 AI 输入通道，以及不会阻塞主视频流的 OSD。核心约束是：

- 主视频始终走 `VI -> VPSS main -> VENC` 硬件绑定链路。
- NPU 推理速度、AI 进程退出、Lua 错误和 AI IPC 拥塞均不能阻塞 VENC。
- AI 输入尺寸、帧率、通道、缓存深度和适配方式由 Rust 注入，不能固化为
  `640x640` 或 VPSS 通道 1。
- Rust daemon 是配置、项目、模型、进程生命周期、Web API 和 OSD 策略的唯一
  所有者；C++ 进程只防御性校验并执行命令。
- 第一阶段算法为 VisionG v1.2.1 + YOLOv5，但接口不绑定单一模型输入尺寸。
- 默认 OSD 只作为 WebRTC/SSE metadata 发送。只有显式选择
  `embedded_rgn` 时，检测框才进入 WebRTC、RTSP 和 MP4 的编码画面。

本阶段不做：

- 不让 Lua 或 AI worker 直接管理 VI、VPSS、VENC、RTSP 或录制。
- 不使用 Python、pybind11 或进程内热替换解释器。
- 不将 NPU 推理结果用逐帧 RGA 覆盖回主路图像。
- 不承诺在硬件 RGN 中绘制文字；嵌入模式第一版只绘制矩形框。
- 不为受信任局域网管理接口新增认证系统。

## 2. 现状与旧实现结论

当前实现中，Rust `aipc-daemon` 管理 HTTP、SSE、WebSocket、WebRTC、RTSP、录制、
持久化配置和 worker 监督。`media_worker` 独占 ISP/RKMPI，视频链路为
`ISP -> VI -> VPSS -> VENC/H264`，音频链路为 `AI -> AENC/G711A`。Rust 创建
匿名 Unix socketpair，worker FD 3 输出 AIPV/H264，FD 4 输出 AIPA/G711A；Rust
再将有界媒体队列分发给各消费者。

归档分支 `archive-legacy-aipc-deployment-paths` / `visiong` 的有效经验包括：

- 已验证 RV1106 上可以使用同一 VPSS group 的第二通道获得低分辨率帧。
- VisionG 可以加载 YOLOv5 RKNN 并在板端完成推理。
- Web 端项目 CRUD、候选代码加载和失败回滚是可用的产品模型。
- 配置变化时需要显式重配 AI VPSS 通道，而不是沿用默认 1080p 输入。

但旧实现的 VisionG/Python producer 让脚本自行抓帧、推理、提交处理后帧并编码，
同时将 VPSS 到 VENC 的主链路解绑。这样 NPU、Python、OSD 或用户代码的任何停顿
都会直接降低或阻塞主视频流；脚本还能创建相机和分发服务，硬件所有权不清晰。
归档中的解释器初始化、GIL、`exec`、cleanup 和线程销毁问题也表明，长生命周期
媒体进程不适合承载可变的 Python 运行时。

新设计保留“项目管理、校验、last-good 回滚和 VisionG 推理”能力，但把 AI 放入
独立进程；media worker 的主视频链路永不因 AI 模式改变。

## 3. 进程与故障边界

```text
                      +---------------- WebSocket preview
                      +---------------- WebRTC H264/PCMA
VI -> VPSS ch(main) -> VENC -> AIPV -> Rust daemon -> RTSP
   |                                      |          + MP4 recording
   +-> VPSS ch(AI) -> AIPF -> latest-only +-> ai_worker -> AIPR
                                               |
                                               +-> Lua + VisionG + RKNN

AIPR -> Rust tracker/interpolator -> WebRTC DataChannel / SSE
                              \----> media control -> hardware RGN (optional)
```

### 3.1 所有权

- `aipc-daemon`
  - 保存 desired、active 和 last-good AI 项目状态。
  - 创建 media 与 AI IPC，转发最新 AI 帧并丢弃过期帧。
  - 监督 `media_worker` 和 `ai_worker`，但两者生命周期相互独立。
  - 解析、校验并跟踪 AIPR，向浏览器或 RGN 后端分发。
- `media_worker`
  - 独占 ISP、VI、VPSS、VENC、AI、AENC 和 RGN 句柄。
  - 主通道继续硬件绑定 VENC。
  - AI 通道使用独立抓帧线程；只负责输出 NV12 和接受 RGN 坐标。
- `ai_worker`
  - 不打开摄像头、不调用 VPSS/VENC/RGN。
  - 从 FD 3 读取 AIPF，从 FD 4 输出 AIPR，日志输出 stderr。
  - 每个进程只加载一个不可变项目快照；部署新脚本时重启 AI 进程。

### 3.2 故障隔离

- media 到 Rust 的 AI 输出为非阻塞、有界、latest-only。写端拥塞时增加 drop
  计数并丢弃整帧，不等待 AI。
- Rust 始终持续排空 media AI 输入，并只向 AI 保存一个待处理帧。AI 退出后
  media 的 FD 5 和 AI VPSS 通道仍可保持运行，重启 AI 不重启 media。
- AI 子进程的启动超时、推理超时、异常退出和无效 AIPR 只改变 AI 状态。
- embedded RGN 的旧检测结果达到 TTL 后自动隐藏；AI 退出不能遗留永久框。
- 只有主 H264 keyframe ready 才决定 media generation 是否健康；AI ready 不参与
  主媒体 worker 的健康判定。

## 4. 动态 VPSS AI 通道

Rust 配置和 AI manifest 共同定义以下结构，部署时 manifest 的值成为 active AI
输入配置：

```json
{
  "enabled": true,
  "channel_id": 1,
  "width": 640,
  "height": 640,
  "fps": 10,
  "pixel_format": "nv12",
  "fit_mode": "contain",
  "buffer_count": 3,
  "depth": 2
}
```

约束：

- `channel_id` 必须是 SDK 支持的 VPSS 通道，且不能等于主通道。
- NV12 宽高必须为正偶数并受板端最大尺寸限制。
- FPS 不得高于主路 FPS；depth 在 SDK 的 0..8 范围内。
- `stretch` 直接缩放到目标尺寸。
- `contain` 保持宽高比并 letterbox，输出仍为 manifest 指定尺寸。
- `cover` 居中裁剪后缩放到目标尺寸。
- AIPF 携带从 AI 输入坐标映射回主路坐标所需的 crop/pad/scale 信息；检测结果
  在 Rust 中统一转换成 0..1 主路归一化坐标。

AI 抓帧线程调用 `RK_MPI_VPSS_GetChnFrame` / `ReleaseChnFrame`。获取帧后先复制到
有界用户态 buffer，再立即 release RKMPI 帧；FD 写入不能持有 RKMPI buffer。
队列容量固定为 1，新的完整帧替换尚未开始发送的旧帧。

### 4.1 在线重配置状态机

部署不同输入要求的项目时：

1. Rust 标记 AI 为 transitioning，并停止候选 AI worker。
2. 向 media 发送 `PauseAiFrames`，等待抓帧线程确认不再持有 VPSS frame。
3. 仅对 AI 通道调用 DisableChn、SetChnAttr、SetChnParam、EnableChn。
4. 恢复抓帧并获取一帧，校验尺寸、stride 和格式后返回 `AiInputReady`。
5. Rust 启动候选项目，要求收到 `WorkerReady` 和首次成功 `InferenceResult`。
6. 成功后原子提交 active/last-good；失败则按同一流程恢复旧通道配置并启动旧
   last-good 项目。

重配置失败不能使用“重启整个 media worker”作为回退手段，也不能 Disable 主 VPSS
通道或重新创建 VENC。

## 5. IPC 协议

所有二进制头使用固定字节序、显式长度、magic 和 version。接收方必须限制 payload
尺寸，并在 magic/长度错误时丢弃连接，避免在原始 NV12 中搜索 magic 造成错误重同步。

### 5.1 FD 分配

| 进程 | FD | 方向 | 协议 |
| --- | ---: | --- | --- |
| media worker | 3 | media -> Rust | AIPV H264 |
| media worker | 4 | media -> Rust | AIPA G711A |
| media worker | 5 | media -> Rust | AIPF NV12 |
| media worker | 6 | 双向 | AIMC 控制 |
| ai worker | 3 | Rust -> AI | AIPF NV12 |
| ai worker | 4 | AI -> Rust | AIPR result/event |

Rust 是 AIPF 中继点，而不是将 media FD 直接接到 AI。这样 AI 每次重启只需替换
Rust 到 AI 的 socketpair，不影响 media 进程的 FD 和 VPSS 生命周期。

### 5.2 AIPF

AIPF header 至少包含：

- magic、version、header length、payload length。
- sequence、monotonic PTS microseconds。
- AI width/height、Y/UV stride、NV12 fourcc。
- main width/height。
- fit mode，以及 crop rectangle、pad rectangle 或等价仿射变换。

payload 是紧随 header 的完整 NV12。第一版允许用户态复制；不得把 RKMPI buffer
生命周期暴露给另一进程。Rust 和 AI 都只允许一个组装中的帧及一个待处理帧，避免
在 174 MB 内存设备上按帧率积压大块 buffer。

### 5.3 AIMC media 控制

AIMC 是长度前缀 JSON 控制消息，低频且双向：

- `pause_ai_frames`
- `configure_ai_channel`
- `resume_ai_frames`
- `set_osd_mode`
- `update_regions`
- `probe_region_capability`
- `ack`、`ai_input_ready`、`region_capability`、`error`

每条命令包含 request ID，Rust 只接受对应 ACK。超时或 worker generation 不匹配时
命令失败。`update_regions` 包含 generation、timestamp、TTL、主路尺寸和固定上限的
矩形列表。

### 5.4 AIPR

AIPR 使用长度前缀 JSON，消息类型包括：

- `worker_ready`：项目、模型、输入要求和 VisionG 版本。
- `inference_result`：输入 sequence/PTS、推理耗时、检测数组。
- `worker_error`：阶段、可恢复性和错误文字。
- `metrics`：读取、处理、丢帧、无效结果和平均耗时计数。

检测项包含 class ID、label、confidence 和 AI 输入坐标。Rust 根据相同 sequence 的
AIPF transform 映射为主路归一化坐标，再为输出分配短生命周期 track ID。

## 6. VisionG 和 Lua 运行时

### 6.1 依赖

- 固定 VisionG release v1.2.1。
- `visiong_cpp.zip` SHA-256：
  `56336cc25150692e21505626b9f359b5dfeaa019f240460c2541b0bfdbe51bc0`。
- YOLOv5n COCO80 RKNN SHA-256：
  `083b2cf8983a9956cb203b3cce1bb83e26690cc9429c7e07d2fd337b06fcccec`。
- COCO80 labels SHA-256：
  `d7654b26101572841ed1cd80aa03aa60e35f1b8acb4aea6906c4066886f16e07`。
- VisionG 以动态库部署到包内 `lib`，`ai_worker` 使用相对 RPATH。保留
  LGPL-3.0-or-later license、版本、下载地址和校验值。
- Lua 固定版本源码静态链接到 `ai_worker`，不依赖板端系统 Lua；保留 Lua license。

第三方获取脚本只写构建缓存，必须验证 SHA-256 后才能参与构建。模型不编进 Rust
或 C++ 二进制，部署在持久化 data 目录。

### 6.2 项目布局和 manifest

```text
data/ai/
  models/
    yolov5n_coco80_640.rknn
    coco80.txt
  projects/<safe-project-id>/
    manifest.json
    main.lua
  deployments/<generation>/
    manifest.json
    main.lua
  state.json
```

manifest v2 包含项目 ID/名称、入口、算法、主模型、`files` 资源映射、`options` 参数、
完整 AI input，以及 threshold、class filter、max detections。ID、资源 role 和相对路径必须通过 allowlist 校验，
解析后的路径必须位于 `data/ai` 内。活动项目运行的是不可变 deployment snapshot，
编辑项目不能隐式改变正在运行的脚本。

当前标准算法适配包括 `yolov5`、`yolo11`、`lprnet`、`mlsd`、`ppocr`、`nanotrack`、
`find_blobs`、`ive_filter`、`ive_ncc`、`npu_clock` 和 `frame_info`。PPOCR 的识别模型/
字典、NanoTrack 的 search/head 三件套都通过 `files` 显式声明，宿主在 worker 启动前
检查所有资源。模型后端统一返回带 `x1/y1/x2/y2/confidence/class_id/label/kind` 的
JSON 对象，算法专属字段由 `annotations` 透传到标准 CloudEvent。

### 6.3 Lua 契约和沙箱

脚本至少定义 `process(frame)`，可选定义 `init(config)` 和 `shutdown()`。`frame`
只暴露元数据和由宿主控制的推理句柄，不将裸指针暴露给 Lua。宿主模块 `aipc` 提供：

- `aipc.load_model(relative_path, options)`
- `aipc.infer(frame, model, options)`（通用 VisionG 后端；`run` 是同义词）
- `aipc.detect(frame, model, options)`
- `aipc.frame_info(frame)`
- `aipc.log(level, message)`

宿主移除 `os`、`io`、`package`、`debug` 和动态库加载能力；项目不能打开任意路径、
启动进程或访问网络。限制 Lua 返回检测数量、字符串长度、嵌套深度和单帧执行错误
次数。单帧 Lua 异常产生 `worker_error`；连续错误超过阈值时 AI 进程退出，由 Rust
执行 last-good 恢复。

对已知会让特定板载 RKNN runtime 复位的模型，manifest 可设置 `options.runtime_guard`；
daemon 要求人工确认 `runtime_guard_ack=true` 才允许在线部署。这样模型仍可被上传、
校验和离线集成，但不会因一次误点部署拖垮主媒体进程或整板。

部署顺序为：manifest 校验、Lua 语法校验、模型存在及 hash 校验、创建不可变快照、
在线重配 AI VPSS、启动候选 AI、等待 ready 和首个成功推理、原子提交 state。

## 7. OSD 设计决定

OSD 枚举为 `off`、`metadata`、`embedded_rgn`，持久化默认值为 `metadata`。

### 7.1 未采用逐帧 RGA

编码前 RGA 合成需要把 AI 结果重新进入主帧处理路径；旧实现还需要解绑 VPSS-VENC。
当 NPU 或脚本变慢时，VENC 没有新帧可编码。即使加入队列，RGA 写入主 buffer 仍会
破坏主链路所有消费者的隔离，因此本设计禁止该路径。

### 7.2 metadata 模式

浏览器在生成 SDP offer 前创建：

```js
pc.createDataChannel("aipc-ai", { ordered: false, maxRetransmits: 0 })
```

str0m 接受该协商通道并向每个 peer 写入紧凑 JSON。消息包含 version、sequence、
PTS、主路尺寸、推理耗时，以及 track ID/class/label/confidence/归一化 box。无
DataChannel 的 WebSocket/MSE 预览使用 `/api/v1/ai/events` SSE 接收同一结构。

浏览器在透明 canvas 上绘制。每个 track 保存最近两个样本，render timestamp 相对
当前时间保留约一个结果间隔的缓冲；在两个样本间线性插值，样本不足时根据上一段
速度最多外推 150 ms，超过 300 ms 未更新则淡出并删除。无法使用 track ID 时用
同类别且 IoU 大于阈值的框做贪心关联。canvas 使用视频实际显示区域进行坐标映射，
正确处理 `object-fit` 黑边。metadata 开关是每客户端状态，不修改服务端全局模式；
RTSP 和 MP4 保持干净。

### 7.3 embedded_rgn 模式

Rust tracker 使用相同的 PTS/IoU 数据，以固定更新频率产生主路像素坐标，通过 AIMC
发送给 media worker。media 启动时探测 RGN：优先尝试 VENC `LINE_RGN`；不支持时
使用每个框四条 `COVER_RGN` 边。按 `max_detections * 4` 上限预创建、attach 并复用
句柄，结果变化时只 SetDisplayAttr/Show，不逐帧 Create/Destroy。

RV1106 真机验证表明，VENC `LINE_RGN` 不支持，VENC/主 VPSS `COVER_RGN` 虽可能
返回成功但不会进入编码画面；Rockchip RV1106 SDK 示例也只将 COVER 挂到 VI。
因此 RV1106 的已验证后端为 `COVER_RGN@VI`，状态 API 必须同时报告 `backend` 和
`target`，不能把仅 API 成功当成可见能力。VI fallback 仍是硬件叠加，不进入 CPU、
RGA 或 NPU 同步路径，因此 WebRTC、RTSP 和 MP4 都包含框，主路保持约 30 FPS。
代价是 VI 后的 AI VPSS 旁路也可能看到上一时刻的 8 像素边框；默认继续使用
`metadata`，只有明确需要 RTSP/MP4 带框时才启用该全局模式。

`off`、AI 退出、generation 改变或 TTL 过期时隐藏所有句柄。第一版不在 RGN 中
绘制 label/confidence；浏览器仍可选择额外显示 metadata 文本。

## 8. Rust API、状态和 Web UI

新增 API：

- `GET /api/v1/ai/status`
- `GET /api/v1/ai/projects`
- `POST /api/v1/ai/projects`
- `GET|PUT|DELETE /api/v1/ai/projects/{id}`
- `POST /api/v1/ai/projects/{id}/validate`
- `POST /api/v1/ai/projects/{id}/deploy`
- `GET /api/v1/ai/models`
- `POST /api/v1/ai/models`，multipart 原子上传
- `DELETE /api/v1/ai/models/{name}`
- `GET /api/v1/ai/osd`
- `PUT /api/v1/ai/osd`，body 为 `{ "mode": "..." }`
- `GET /api/v1/ai/events`，SSE metadata fallback
- `GET /api/v1/ai/results/latest`，最新标准 CloudEvent
- `GET /api/v1/ai/results/stream`，支持 `Last-Event-ID` 的 SSE/replay
- `GET /api/v1/ai/results/schema`，AI 结果 JSON Schema

AI status 至少返回 enabled/state、active/last-good project、generation、active input、
worker PID、ready、last result time、inference FPS/latency、media/daemon/AI drop 计数、
RGN capability、标准结果总线状态和 last error。删除活动或 last-good 引用的模型必须
返回冲突。外部服务接口的 CloudEvents、坐标和 replay 契约见
[`ai_result_api.md`](./ai_result_api.md)。

Web UI 提供 AI 状态、项目列表、Lua/manifest 编辑、校验、部署、模型上传/删除、
推理指标、错误和 OSD 三态控制。文件先写 `.part` 并 fsync/rename，失败不得覆盖
原项目或模型；HTTP 层限制 body 大小并拒绝路径分隔符。

## 9. 构建、部署和数据保留

`scripts/build-rv1106.sh` 同时交叉编译 Rust daemon、media worker 和 ai_worker。
`scripts/package-rv1106.sh` 打包：

```text
bin/aipc-daemon
bin/media_worker
bin/ai_worker
lib/<VisionG shared libraries>
www/<Vue bundle>
config/<default config>
licenses/visiong/
licenses/lua/
```

默认示例项目可随包放入 seed 目录，但部署时只在目标不存在时复制到
`/root/aipc-rust/data/ai`。现有 data、项目、模型、state、录制和日志不能被包覆盖。
部署前保留上一版可启动的 bin/lib，失败时恢复它；只停止 package 自身的
`aipc-daemon`、`media_worker` 和 `ai_worker`。

板端固定调试序列号为 `51b2f225656e6459`。每次部署前仍通过 ADB 重新读取地址和
路由。优先从以太网 `192.168.100.106` 验证 HTTP/RTSP，Wi-Fi
`192.168.100.249` 为第二路径；DHCP 地址只有当次探测确认后才使用。板上约 174 MB
RAM，禁止在 `/tmp` 累积 NV12 dump 或模型副本。

## 10. 分阶段实施和验收矩阵

### 阶段 A：动态 VPSS 与 AIPF

- host 单元测试覆盖 config 默认值、边界、主/AI 通道冲突和 AIPF 编解码。
- 板端探测 640x360、640x640、不同 FPS/depth，校验真实帧尺寸/stride/PTS。
- 在 Rust 不读取、AI 不存在和频繁重配置时，主路 H264、RTSP、WebRTC 和录制继续
  工作，VENC channel 不重建。

### 阶段 B：AI worker、Lua 和 VisionG

- host 用 mock backend 测 Lua sandbox、manifest、AIPR、超时和 latest-only。
- 板端加载 YOLOv5n 模型并获得首个有效结果。
- 脚本语法错误、模型损坏、NPU 错误和进程退出触发 last-good，media PID 和主路
  generation 保持不变。

### 阶段 C：metadata 与 Web 管理

- API 测试覆盖 CRUD、原子写、非法路径、活动模型删除冲突和部署回滚。
- Web 测试覆盖 DataChannel 创建时机、SSE fallback、坐标映射、插值、过期淡出和
  每客户端开关。
- metadata 开启时 RTSP 和新录制 MP4 不含框。

### 阶段 D：硬件 RGN

- 板端记录 LINE/COVER capability、实际 attach target，并用编码抓帧验证可见性。
- 测试框移动、数量变化、句柄复用、TTL 清屏及 off/metadata/embedded 切换。
- embedded 模式下 WebRTC、RTSP、MP4 均有框，切回 off 后均恢复干净画面。

### 阶段 E：完整回归

- `/healthz`、`/api/v1/status`、preview ready、WebRTC、RTSP TCP/UDP、录制启停、
  HTTP HEAD/Range 和拉取后 ffprobe 全部通过。
- 同时运行 preview、RTSP、录制和 AI，检查慢客户端隔离。
- 持续运行期间检查 RSS、FD、VPSS/RGN 句柄、NPU 错误、drop 指标和自动恢复。
- 验收底线：AI 可降帧、退出或回滚，但不能造成主视频停顿、VENC 重建或录制中断。

## 11. RV1106 真机验证记录

2026-08-07 在设备 `51b2f225656e6459` 完成以下验证：

- `640x360/stretch` 与 `640x640/contain` 可在线切换；contain 使用 VPSS 内容缩放加
  AI 抓帧线程 NV12 letterbox，未再使用会触发内核异常的
  `VPSS_ASPECT_RATIO_AUTO`。
- VisionG v1.2.1 加载 `yolov5n_coco80_640.rknn` 成功，640x640 单帧推理约
  71--94 ms；最终持续运行平均约 75.4 ms、结果速率约 10 FPS。
- Lua 连续异常、候选部署失败、`ai_worker` 被 `SIGKILL`、media worker 单独重启
  均可恢复；AI 重启不要求 media 重启，media generation 变化后 Rust 会按 active
  manifest 重配新的 AI 通道。
- VENC `LINE_RGN` 不支持；VENC/主 VPSS `COVER_RGN` 更新不可见；
  `COVER_RGN@VI` 的四边框在 RTSP 抓帧和 MP4 录制中可见，切回 metadata 后清屏。
  主路在 embedded 模式仍约 30 FPS，未出现新增 RGA Oops 或 RGN 更新错误。
- HTTP health/status、WebRTC H264/PCMA、RTSP TCP/UDP、Wi-Fi 第二路径、录制启停、
  HEAD/Range 206 与下载后 ffprobe 均通过。
- 10 分钟连续采样期间 media generation、media PID、AI PID 均未变化；主路
  29.85--30.12 FPS，video/AI error、timeout、IPC drop 和 malformed frame 均为 0。
  结束时 daemon/media/AI RSS 约为 14.5/7.7/8.3 MiB，FD 数为 26/63/14。
