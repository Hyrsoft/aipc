# AIPC 项目 — Agent 开发标准

本文件是面向 AI coding agent 的项目级工作规范。Agent 在本仓库中进行分析、实现、调试、提交时，应优先遵循本文档；若与用户本轮明确要求冲突，以用户最新要求为准。

## 项目概述

AIPC（Edge AI Camera）是运行在 **Luckfox Pico** 嵌入式开发板上的边缘 AI 摄像头应用。
目标板卡 CPU 为单核 ARM，运行 uclibc Linux。开发机为 Arch Linux x86_64，通过交叉编译构建目标产物，再通过 rsync 部署到板端。

核心功能：
- 双模生产者架构：**SimpleIPC**（纯硬件绑定，零拷贝）和 **VisionG/Python**（AI 推理，Python 驱动帧循环）
- 流媒体分发：RTSP / WebRTC / WebSocket 预览 / 本地录制
- HTTP API（端口 8080）提供 Web UI 和 REST 控制接口
- Python 脚本热部署：通过 Web UI 上传并实时切换 Python 工程

---

## 仓库结构

```
aipc/
├── src/
│   ├── main.cpp                        # 入口：日志初始化、Python 预热、信号处理、服务启动
│   ├── http.cpp / http.h               # HTTP API 路由（cpp-httplib）
│   ├── common/                         # 日志、工具类
│   ├── media_producer/
│   │   ├── i_media_producer.h          # 生产者接口
│   │   ├── media_manager.cpp/h         # 生产者生命周期管理、模式切换
│   │   ├── simple_ipc/                 # SimpleIPC 生产者（RKMPI 硬件绑定）
│   │   └── visiong/
│   │       ├── visiong_producer.cpp/h  # VisionG 生产者 + PythonRuntime
│   └── media_distribution/
│       ├── rtsp/                       # RTSP 服务
│       ├── webrtc/                     # WebRTC 服务（libdatachannel）
│       ├── wspreview/                  # WebSocket 预览
│       └── file/                       # 本地录制
├── assets/
│   ├── install_rsync.sh                # 构建 + cmake install + rsync 到板端
│   ├── start_app.sh                    # 板端启动脚本
│   ├── stop_app.sh                     # 板端停止脚本
│   ├── build_frontend.sh               # 前端构建
│   └── python_projects/               # 内置 Python 工程示例
├── docs/
│   ├── coding_style.md                 # 代码规范（Google C++ Style）
│   ├── VisionG-学习者-Wiki.md          # VisionG API 学习资料，含 AIPC 集成约束
│   ├── 关于动态库.md                   # 板端动态库部署说明
│   └── visiong库移植/                  # VisionG 移植笔记
├── docs/agent_auto_debug_skill/         # Agent 调试 Skill 和少量 repo 快照
├── build/Debug/                        # 示例 CMake 构建目录（交叉编译）
├── CMakeLists.txt
└── CMakePresets.json
```

---

## 技术栈

| 层次 | 技术 |
|------|------|
| 语言 | C++17，少量 C11（SDK 胶水层） |
| 构建 | CMake 3.15+，交叉编译工具链（arm-rockchip）|
| Python 嵌入 | pybind11 embedded interpreter（Python 3.11） |
| HTTP 服务 | cpp-httplib（单头文件，内嵌 Web UI） |
| WebRTC | libdatachannel |
| 日志 | spdlog，宏 `LOG_DEBUG/INFO/WARN/ERROR`，文件头定义 `LOG_TAG` |
| 硬件 SDK | Luckfox RKMPI（RK_MPI_VI/VPSS/VENC） |
| AI 推理 | VisionG（预编译库，Python 侧 `visiong.Camera / visiong.NPU`） |

---

## 代码规范

遵循 `docs/coding_style.md`，核心要点：

- **文件名**：全小写 + 下划线，`.h` / `.cpp`
- **类型名**：PascalCase
- **函数名**：PascalCase（类方法），底层 SDK 封装用 snake_case
- **成员变量**：`snake_case_`（尾部下划线）
- **全局变量**：`g_` 前缀 + snake_case
- **常量**：`kPascalCase`
- **头文件保护**：`#pragma once`
- **智能指针**：优先 `std::unique_ptr`，共享所有权用 `std::shared_ptr`
- **锁**：`std::lock_guard` / `std::unique_lock`，明确标注线程安全性
- **错误处理**：返回值 + 日志，关键路径记录 `LOG_ERROR`

## Agent 工作准则

- 先读代码再改动，优先使用 `rg`、`sed`、`git diff` 获取上下文。
- 不回滚用户已有改动；遇到不相关脏文件时只忽略，不清理。
- 修改嵌入式运行路径、部署脚本、CMake 时，优先使用可配置变量，避免写死开发机路径、板端 IP 或用户名。
- 变更后至少执行 `cmake --build "${AIPC_BUILD_DIR:-build/Debug}"`，涉及前端时再执行 `npm run build` 或 `./assets/build_frontend.sh`。
- 涉及板端行为时，优先使用 `.github/skills/aipc-agent-auto-debug/SKILL.md` 中的 ADB/网络调试流程。
- 提交信息必须遵循 `docs/git_commit_convention.md`。

---

## 架构关键约束

### 双模切换（冷切换）

`MediaManager::SwitchMode()` 执行完整的 Stop → Deinit → 创建新生产者 → Init → Start 流程，约耗时 100–300ms。不支持热切换。

### VisionG/Python 并发安全

`PythonRuntime` 内部锁顺序规则（**必须严格遵守，否则死锁**）：

```
正确顺序：GIL → mutex_
```

所有涉及 Python API 的路径（`LoadCode`、`Shutdown`、`CallRun`、构造函数）都必须先获取 GIL，再获取 `mutex_`。反向顺序会导致与帧循环线程形成循环等待。

### Python 解释器初始化

`WarmupVisionGPythonRuntime()` 在 `main()` 早期调用，通过 `std::call_once` 保证只初始化一次。`initialize_interpreter` 完成后必须调用 `PyEval_SaveThread()` 交还 GIL，否则 main 线程会永久持有 GIL，后续所有工作线程的 `gil_scoped_acquire` 会死等。

### Python 脚本契约

Python 脚本必须导出 `run()` 函数（必须），`init()` 和 `cleanup()` 可选：

```python
import visiong
import aipc

def init():    # 可选，初始化摄像头/模型
    pass

def run():     # 必须，帧循环直到 aipc.is_running() 返回 False
    while aipc.is_running():
        frame = ...
        aipc.submit_frame(frame)

def cleanup(): # 可选，释放资源
    pass
```

---

## 构建与部署

### 构建

```bash
# 构建目录可用 AIPC_BUILD_DIR 覆盖
cmake --build "${AIPC_BUILD_DIR:-build/Debug}"
```

### 部署到板端

```bash
# 一键：构建前端 + cmake install + rsync 增量同步
./assets/install_rsync.sh
```

部署目标由环境变量控制：

```bash
export AIPC_REMOTE_HOST="${AIPC_REMOTE_HOST:-root@192.168.8.235}"
export AIPC_REMOTE_DIR="${AIPC_REMOTE_DIR:-/root/aipc}"
```

### 板端操作

```bash
# SSH 登录
ssh "$AIPC_REMOTE_HOST"

# 启动（前台）
cd "$AIPC_REMOTE_DIR/bin" && ./start_app.sh

# 启动（后台）
cd "$AIPC_REMOTE_DIR/bin" && ./start_app.sh --daemon

# 停止
./assets/stop_app.sh

# 查看日志（后台模式）
tail -f /var/log/aipc.log
```

---

## HTTP API 速查

基础地址：`${AIPC_HTTP_BASE:-http://192.168.8.235:8080}`

| 方法 | 路径 | 说明 |
|------|------|------|
| GET  | `/api/status` | 整体状态（生产者模式、流服务状态）|
| POST | `/api/ai/switch` | 切换 AI 模式，body: `{"model":"visiong"\|"none"}` |
| GET  | `/api/python/status` | Python 运行状态、最近错误 |
| GET  | `/api/python/projects` | 列出所有 Python 工程 |
| POST | `/api/python/projects/create` | 创建工程，body: `{"name":"xxx","code":"..."}` |
| GET  | `/api/python/projects/{name}` | 读取工程代码 |
| POST | `/api/python/projects/{name}` | 更新工程代码 |
| DELETE | `/api/python/projects/{name}` | 删除工程 |
| POST | `/api/python/deploy` | 部署工程，body: `{"project":"xxx.py"}` |
| POST | `/api/producer/switch` | 切换生产者模式 |
| GET  | `/api/rtsp/status` | RTSP 状态 |
| POST | `/api/rtsp/start` | 启动 RTSP |
| POST | `/api/rtsp/stop` | 停止 RTSP |
| GET  | `/api/webrtc/status` | WebRTC 状态 |
| POST | `/api/webrtc/start` | 启动 WebRTC |
| POST | `/api/webrtc/stop` | 停止 WebRTC |

---

## 已知问题与修复记录

### [已修复] PythonRuntime 锁顺序死锁

**现象**：切换到 VisionG 模式后，`script_thread` 执行 `run()` 期间，HTTP 线程调用 `LoadCode` 或 `Shutdown` 导致循环等待卡死。

**根因**：`LoadCode` 和 `Shutdown` 加锁顺序为 `mutex_ → GIL`，与 `CallRun` 的 `GIL → mutex_` 反向。

**修复**：统一为 `GIL → mutex_`。

### [已修复] main 线程永久持有 GIL

**现象**：`WarmupVisionGPythonRuntime()` 后，HTTP 工作线程触发 `new PythonRuntime()` 卡在 `ctor begin`，永不打印 `ctor done`。

**根因**：`py::initialize_interpreter` 执行后调用线程自动持有 GIL，`WarmupVisionGPythonRuntime` 返回后 main 线程进入事件循环，GIL 永不释放。

**修复**：在 `EnsureEmbeddedPythonReady` 的 `call_once` lambda 末尾调用 `PyEval_SaveThread()`，让解释器进入多线程待机状态。

---

## Agent 调试指引

详见 `.github/skills/aipc-agent-auto-debug/SKILL.md`。

快速自检清单（卡死/崩溃时）：

1. `ssh "$AIPC_REMOTE_HOST" 'pgrep aipc &&
