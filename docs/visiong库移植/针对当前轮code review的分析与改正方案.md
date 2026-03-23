# 针对当前轮 code review 的分析与改正方案

> 对应 review 文档：`docs/visiong库移植/人工code review 结果.md`
> 分析时间：2025-07

---

## 问题 1：分辨率配置不应作为 media producer 共有接口

### 现状分析

#### 1.1 `ProducerConfig` 中遗留的 `ai_width/ai_height` 字段

**文件：** `src/media_producer/i_media_producer.h`

```cpp
struct ProducerConfig {
    Resolution resolution = Resolution::R_1080P;
    int framerate = 30;
    int bitrate_kbps = 10 * 1024;

    // AI 相关（仅对 AI 模式有效）
    // AI 摄像头采集尺寸（VisionG 模式，Phase B：由 C++ Camera 使用）
    // 16:9 宽高比，与摄像头硬件输出匹配
    int ai_width = 640;
    int ai_height = 360;
    ...
};
```

`ai_width/ai_height` 是 Phase B 遗留字段——在 Phase B 中 C++ 负责创建摄像头，因此需要在 `ProducerConfig` 中存储摄像头尺寸。当前目标架构已确定由 **Python 代码**负责摄像头初始化（`visiong.Camera`），C++ 不再拥有摄像头对象，这两个字段对 VisionG 模式已无意义，但它们至今仍作为 `ProducerConfig` 的共有成员对外暴露。

连带地，`VisionGProducer::Init()` 中直接读取这两个字段来创建 C++ `Camera`：

```cpp
// src/media_producer/visiong/visiong_producer.cpp  Init()
const int cam_w = config_.ai_width;   // 640
const int cam_h = config_.ai_height;  // 360
...
auto camera = std::make_unique<Camera>(cam_w, cam_h, "rgb");
```

这说明整个 VisionGProducer 的 `Init` 逻辑仍是 Phase B 架构——C++ 持有摄像头，而非 Python。

#### 1.2 `Resolution` 枚举与 `ResolutionConfig` 结构体暴露在共有接口中

**文件：** `src/media_producer/i_media_producer.h`

```cpp
enum class Resolution {
    R_1080P, R_720P, R_480P,
};

struct ResolutionConfig {
    int width = 1920;
    int height = 1080;
    int framerate = 30;
    static ResolutionConfig FromPreset(Resolution preset) { ... }
};
```

这套预设分辨率体系（1080P / 720P / 480P）仅对 **SimpleIPC** 有实际意义——SimpleIPC 通过 `RK_MPI_VI/VPSS` 接口设置硬件分辨率，分辨率切换是一个明确的、有限的枚举选项。

VisionG 模式下，摄像头分辨率完全由 Python 脚本决定（`visiong.Camera(width, height, ...)`），与这套枚举毫无关联。将它们定义在共有头文件 `i_media_producer.h` 中，会给阅读者造成"两种模式都需要用分辨率预设"的误导。

#### 1.3 `IMediaProducer::SetResolution()` 和 `SetFrameRate()` 作为共有虚接口

**文件：** `src/media_producer/i_media_producer.h`

```cpp
// ========== 可选配置接口 ==========
virtual int SetResolution(Resolution preset) {
    (void) preset;
    return -1;  // 默认不支持
}
virtual int SetFrameRate(int fps) {
    (void) fps;
    return -1;  // 默认不支持
}
```

`VisionGProducer` 覆写了这两个方法，但实现形同虚设：

```cpp
// src/media_producer/visiong/visiong_producer.cpp
int VisionGProducer::SetResolution(Resolution preset) {
    config_.resolution = preset;  // 存储但不生效
    return 0;
}
int VisionGProducer::SetFrameRate(int fps) {
    config_.framerate = std::max(1, std::min(fps, 30));  // 存储但不生效
    return 0;
}
```

这违反了接口设计原则：接口上存在一个方法，某个子类实现它但它毫无实际效果，却返回成功（`return 0`），会让调用方误以为操作成功。

#### 1.4 `MediaManager::SetResolution()` 对 VisionG 模式没有意义

**文件：** `src/media_producer/media_manager.cpp`

```cpp
int MediaManager::SetResolution(Resolution preset) {
    ...
    // 需要重新初始化
    bool was_running = producer_->IsRunning();
    if (was_running) producer_->Stop();
    producer_->Deinit();
    config_.resolution = preset;
    if (producer_->Init() != 0) { ... }
    ReregisterConsumers();
    if (was_running) producer_->Start();
    ...
}
```

这个"Stop -> Deinit -> 改 config -> Init -> Start"的重初始化流程完全是为 SimpleIPC 的硬件分辨率切换设计的。若在 VisionG 模式下调用，会触发无意义的重初始化。`MediaManager` 没有对当前模式进行判断和拒绝，任何模式下都可调用。

#### 1.5 `http.cpp` 中的相关端点

**文件：** `src/http.cpp`

`/api/pipeline/status` 在 VisionG 模式下仍读取 `ProducerConfig` 中的 `ai_width/ai_height`：

```cpp
auto vg_cfg = mgr.GetConfig();
json cam_info;
cam_info["width"] = vg_cfg.ai_width;   // Phase B 遗留
cam_info["height"] = vg_cfg.ai_height; // Phase B 遗留
cam_info["format"] = "rgb";
data["camera"] = cam_info;
```

`/api/python/status` 同样报告：

```cpp
json cam_info;
cam_info["managed_by"] = "c++";        // Phase B 遗留描述
cam_info["width"] = cfg.ai_width;
cam_info["height"] = cfg.ai_height;
```

### 改正方案

#### 方案 A：将分辨率相关定义下移至 SimpleIPC 专属作用域

**Step 1：** 将 `Resolution` 枚举、`ResolutionConfig` 结构体从 `i_media_producer.h` 移出，迁移至新建的 `src/media_producer/simple_ipc/simple_ipc_config.h`。

```cpp
// src/media_producer/simple_ipc/simple_ipc_config.h
namespace media::simple_ipc {

    enum class Resolution { R_1080P, R_720P, R_480P };

    struct ResolutionConfig {
        int width = 1920;
        int height = 1080;
        int framerate = 30;
        static ResolutionConfig FromPreset(Resolution preset);
    };

}
```

**Step 2：** 将 `ProducerConfig` 中的 `resolution` 字段及 `ai_width/ai_height` 字段全部移除。`ProducerConfig` 只保留两种模式共同需要的参数（`framerate`、`bitrate_kbps`）。SimpleIPC 专用的分辨率配置改由 `SimpleIPCProducer` 构造时单独传入：

```cpp
// i_media_producer.h（简化后）
struct ProducerConfig {
    int framerate = 30;
    int bitrate_kbps = 10 * 1024;
};

// simple_ipc_producer.h
struct SimpleIPCConfig : public ProducerConfig {
    simple_ipc::Resolution resolution = simple_ipc::Resolution::R_1080P;
};
```

**Step 3：** 将 `IMediaProducer::SetResolution()` 和 `SetFrameRate()` 从接口中删除。`SimpleIPCProducer` 提供具体的（非虚的）分辨率切换方法；`VisionGProducer` 不提供此方法。

**Step 4：** `MediaManager::SetResolution()` 改为仅在 SimpleIPC 模式下生效，并通过 `dynamic_cast` 直接调用 `SimpleIPCProducer` 的具体方法，不再走 `IMediaProducer` 虚接口：

```cpp
int MediaManager::SetResolution(simple_ipc::Resolution preset) {
    if (current_mode_ != ProducerMode::SimpleIPC) {
        LOG_WARN("SetResolution ignored: not in SimpleIPC mode");
        return -1;
    }
    auto *sipc = dynamic_cast<SimpleIPCProducer *>(producer_.get());
    if (!sipc) return -1;
    return sipc->SetResolution(preset);  // 调用具体方法
}
```

**Step 5：** 修正 `http.cpp` 中相关端点：
- `/api/pipeline/status` VisionG 分支：删除 `ai_width/ai_height` 读取，改为提示摄像头由 Python 管理。
- `/api/python/status`：`cam_info["managed_by"]` 改为 `"python"`，删除 `width/height` 字段（C++ 不再知晓）。

---

## 问题 2：`mpi_config.h` 名称空间混淆与 AI 遗留配置

### 现状分析

#### 2.1 命名空间使用 `media` 而非 `media::simple_ipc`

**文件：** `src/media_producer/simple_ipc/mpi_config.h`

```cpp
namespace media {

    constexpr int kViDev = 0;
    constexpr int kVpssChn1 = 1;  // VPSS 通道 1（AI 推理）

    inline int vi_dev_init() { ... }
    inline int vi_chn_init(...) { ... }
    inline int vpss_init(...) { ... }
    inline int vpss_init_serial_mode(...) { ... }
    inline int vpss_deinit(...) { ... }
    inline int venc_init(...) { ... }
    inline int venc_init_rgb_input(...) { ... }

} // namespace media
```

这些全部是 SimpleIPC 模式使用的 RKMPI 初始化自由函数，与 VisionG 没有任何关联（VisionG 通过 Python 调用 visiong 自带的 API 进行初始化，不触碰这些函数）。然而它们共享 `namespace media`，与 `IMediaProducer`、`MediaManager`、`ProducerConfig` 等通用类型处于同一名称空间，语义上造成混淆——阅读者很难区分哪些符号是通用的，哪些是 SimpleIPC 专有的。

#### 2.2 `vpss_init_serial_mode()` — Phase B AI 管线遗留

```cpp
inline int vpss_init_serial_mode(int grpId, int width, int height) {
    ...
    stChnAttr.u32Depth = 2; // 允许手动获取
    RK_MPI_VPSS_SetChnAttr(grpId, kVpssChn0, &stChnAttr);
    ...
}
```

此函数配置 VPSS Chn0 为"串行模式"（`u32Depth > 0`，CPU 手动 `GetChnFrame`），是旧架构中 C++ 手动从 VPSS 取帧再送 Python 处理的遗留设计。SimpleIPC 模式的 VPSS 使用"并行模式"（`u32Depth = 0`，硬件直接绑定 VENC），此函数对 SimpleIPC 是死代码，放在此文件中会产生误导。

#### 2.3 `venc_init_rgb_input()` — Phase B VisionG 编码遗留

```cpp
inline int venc_init_rgb_input(int chnId, int width, int height, RK_CODEC_ID_E enType) {
    ...
    stAttr.stVencAttr.enPixelFormat = RK_FMT_RGB888; // RGB 输入
    ...
}
```

此函数以 `RK_FMT_RGB888` 作为 VENC 输入格式，是 Phase B 中 C++ 接收 Python 返回的 RGB 帧并送编码器的遗留逻辑。SimpleIPC 的 VENC 输入为 `RK_FMT_YUV420SP`（NV12），不需要 RGB 输入版本。此函数对 SimpleIPC 同样是死代码。

#### 2.4 `kVpssChn1` 常量 — AI 推理通道遗留

```cpp
constexpr int kVpssChn1 = 1; ///< VPSS 通道 1（AI 推理）
```

注释明确标注了"AI 推理"用途。SimpleIPC 只用 Chn0（绑定 VENC），不存在 AI 推理通道，此常量在 SimpleIPC 上下文中没有使用场景。

#### 2.5 `vpss_init()` 中的 AI 通道可选参数

```cpp
inline int vpss_init(int grpId, int inputWidth, int inputHeight,
                     int chn0Width, int chn0Height,
                     int chn1Width = 0, int chn1Height = 0) {  // AI Chn1 配置
    ...
    // Chn1: AI 推理（可选）
    if (chn1Width > 0 && chn1Height > 0) {
        ...
        stChnAttr.u32Depth = 2; // 允许手动获取
        RK_MPI_VPSS_SetChnAttr(grpId, kVpssChn1, &stChnAttr);
        RK_MPI_VPSS_EnableChn(grpId, kVpssChn1);
    }
}
```

Chn1 的配置逻辑是 Phase B 旧架构（C++ 从 VPSS Chn1 取帧做 AI 推理）的产物。SimpleIPC 调用此函数时无需传入 `chn1Width/chn1Height`（默认值 0 会跳过），但这段配置代码的存在仍会让维护者困惑。

### 改正方案

**Step 1：** 将 `mpi_config.h` 的命名空间从 `media` 改为 `media::simple_ipc`，并在 `simple_ipc_producer.cpp` 中更新所有引用（或在文件顶部加 `using namespace media::simple_ipc;`）：

```cpp
// mpi_config.h
namespace media::simple_ipc {

    constexpr int kViDev   = 0;
    constexpr int kViChn   = 0;
    constexpr int kVpssGrp = 0;
    constexpr int kVpssChn = 0;  // 重命名：SimpleIPC 只有一个 VPSS 通道
    constexpr int kVencChn = 0;

    inline int vi_dev_init() { ... }
    inline int vi_chn_init(...) { ... }
    inline int vpss_init(...) { ... }      // 移除 Chn1 可选参数
    inline int vpss_deinit(...) { ... }    // 移除 enableChn1 参数
    inline int venc_init(...) { ... }

} // namespace media::simple_ipc
```

**Step 2：** 删除以下符号：
- `vpss_init_serial_mode()` — Phase B 遗留，SimpleIPC 不用串行模式
- `venc_init_rgb_input()` — Phase B 遗留，SimpleIPC 不处理 RGB 帧
- `kVpssChn1` — AI 推理通道，SimpleIPC 无此通道

**Step 3：** 简化 `vpss_init()` 签名，移除 `chn1Width/chn1Height` 参数和对应的 Chn1 配置逻辑；简化 `vpss_deinit()` 签名，移除 `enableChn1` 参数。

**Step 4：** 在文件头注释中明确标注适用范围：

```cpp
/**
 * @file mpi_config.h
 * @brief SimpleIPC 模式专用的 RKMPI 初始化辅助函数
 *
 * 注意：本文件仅供 SimpleIPCProducer 使用。
 * VisionG 模式通过 Python 调用 visiong 自带的 API，不使用此文件中的任何函数。
 */
```

---

## 问题 3：`visiong_producer.h` 注释与 `http.cpp` 内嵌 Python 描述的架构已过时

### 现状分析

#### 3.1 目标架构 vs. 当前代码架构

根据 `docs/visiong库移植/思路.md` 明确描述，visiong 模式的目标架构为：

| 职责方 | 负责内容 |
|--------|---------|
| **Python** | 摄像头初始化（`visiong.Camera`）、帧循环（`while True`）、模型加载与推理、分辨率配置、将处理后的帧提交给 C++ 编码 |
| **C++** | Python 解释器生命周期管理、Python 代码的加载与部署、VENC 编码、流媒体分发（RTSP/WebRTC 等） |

示例 Python 工程形态（摘自思路.md）：
```python
import visiong
cam = visiong.Camera(640, 360, format='rgb')
det = visiong.NPU('yolov5', MODEL, LABELS, box=0.25, nms=0.45)
try:
    cam.skip(8)
    while True:
        frame = cam.snapshot()
        if not frame.is_valid():
            continue
        out = frame.to_format('bgr888')
        for r in det.infer(frame, model_format='rgb'):
            ...
        # 将处理后的帧提交给 C++ 进行 VENC 编码
finally:
    cam.release()
```

但当前代码实现的是 **Phase B 架构**（C++ 驱动帧循环，Python 作为插件）：

| 职责方 | Phase B 实际情况 |
|--------|----------------|
| **C++** | 摄像头初始化（`Camera`）、帧循环（`FrameLoop`）、将帧送给 Python、接收结果编码 |
| **Python** | 纯处理插件：实现 `process(frame) -> ImageBuffer \| None` |

#### 3.2 `visiong_producer.h` 注释描述 Phase B 架构

**文件：** `src/media_producer/visiong/visiong_producer.h`

```cpp
/**
 * @file visiong_producer.h
 * @brief VisionG Python 模式生产者（Phase B 架构）
 *
 * Phase B 架构说明：
 *   C++ 负责：摄像头采集、帧循环驱动、Python 代码管理、生命周期管理、编码和分发。
 *   Python 负责：纯处理逻辑（推理 + 绘制），契约为 process(frame) -> ImageBuffer | None。
 *
 * Python 脚本契约：
 *   def init()          可选，模块加载时调用一次（初始化模型等资源）
 *   def process(frame)  必须，每帧由 C++ 调用；返回处理后的 ImageBuffer 或 None（跳过）
 *   def cleanup()       可选，模块卸载时调用一次（释放资源）
 *
 * 不允许在 Python 脚本中自建 Camera 或帧循环主控。   <-- 与目标架构相反
 */
```

- "C++ 负责摄像头采集、帧循环驱动"与目标架构矛盾
- "不允许在 Python 脚本中自建 Camera 或帧循环主控"与目标架构完全相反
- Python 契约 `process(frame)` 是 Phase B 接口，目标架构中 Python 不再被动接收帧

#### 3.3 `visiong_producer.cpp` 中的 `kDefaultVisionGScript` 是 Phase B 透传脚本

**文件：** `src/media_producer/visiong/visiong_producer.cpp`

```python
# kDefaultVisionGScript (Phase B 风格)
def init():
    pass

def process(frame):
    # Passthrough: return the frame as-is.
    return frame

def cleanup():
    pass
```

这段默认脚本实现了 `process(frame)` 接口（C++ 提供帧，Python 处理并返回），属于 Phase B 的合约形式。目标架构中 Python 自己驱动摄像头，不存在这种被动的 `process(frame)` 契约。

#### 3.4 `http.cpp` 中 `DEFAULT_YOLOV5_PROJECT_CODE` 也是 Phase B 风格

**文件：** `src/http.cpp`，约 L38-L116

```python
# DEFAULT_YOLOV5_PROJECT_CODE (Phase B 风格)
def init():
    _detector = visiong.NPU(...)  # 加载模型
    # 注意：不创建 Camera，因为 Phase B 由 C++ 管理摄像头

def process(frame):
    # frame 由 C++ 提供
    out = frame.to_format("bgr888")
    for result in _detector.infer(frame, model_format=CAM_FORMAT):
        out.draw_rectangle(...)
    return out  # 返回给 C++ 编码

def cleanup():
    _detector = None
```

注释"不再需要在 init() 中创建 Camera"、"C++ 已经完成采集，frame 是当前帧"，完全是 Phase B 语境，与目标架构矛盾：目标架构中 Python 应自己创建 Camera，自己驱动帧循环。

此外，文件顶部注释也明确写着 `# Phase B 契约：C++ 驱动帧循环，Python 只负责推理与绘制`，与目标架构相反。

#### 3.5 `http.cpp` 状态端点 `/api/python/status` 报告 Phase B 信息

```cpp
json cam_info;
cam_info["managed_by"] = "c++";    // Phase B 遗留
cam_info["width"] = cfg.ai_width;
cam_info["height"] = cfg.ai_height;
data["camera"] = cam_info;
```

Phase B 中 C++ 持有摄像头，因此这里报告 `managed_by = "c++"`。目标架构中 Python 自管摄像头，C++ 不知道摄像头参数。

#### 3.6 `VisionGProducer` 实现本体仍是 Phase B

从 `visiong_producer.cpp` 实现来看，当前 `VisionGProducer` 完整地实现了 Phase B：

- `Init()`：创建 C++ `Camera` 对象，调用 `camera->skip(8)`
- `FrameLoop()`：C++ 循环调用 `camera->snapshot()`，将帧传给 Python `process(frame)`，接收返回帧后送 VENC 编码

整个实现都与目标架构相违背。

### 改正方案

#### 整体重构方向

VisionGProducer 需要从"C++ 驱动帧循环，Python 作为处理插件"重构为"Python 驱动一切，C++ 提供编码通道"。

**核心机制变化：** 需要通过 pybind11 向 Python 暴露一个帧提交接口（如 `aipc.submit_frame(frame)`），让 Python 脚本在自己的帧循环中将处理后的帧推送给 C++ 进行编码。

#### Step 1：重写 `visiong_producer.h` 注释

删除所有 Phase B 描述，更新为目标架构描述：

```cpp
/**
 * @file visiong_producer.h
 * @brief VisionG Python 驱动模式生产者
 *
 * 架构说明：
 *   Python 负责：摄像头初始化（visiong.Camera）、帧循环驱动、
 *               模型加载与推理、分辨率配置、将处理后的帧通过
 *               aipc.submit_frame() 提交给 C++ 编码。
 *   C++  负责：Python 解释器生命周期管理、Python 工程的加载与
 *               热更新、VENC 编码、流媒体分发（RTSP/WebRTC 等）。
 *
 * Python 脚本的典型结构：
 *   import visiong, aipc
 *
 *   def init():
 *       # 初始化摄像头、加载模型等资源
 *
 *   def run():
 *       # 驱动帧循环，直至 aipc.is_running() 返回 False
 *       while aipc.is_running():
 *           frame = cam.snapshot()
 *           ...
 *           aipc.submit_frame(processed_frame)
 *
 *   def cleanup():
 *       # 释放资源
 *
 * Python 脚本契约：
 *   init()     可选，C++ 在启动前调用一次
 *   run()      必须，C++ 在独立线程中调用，脚本在此驱动帧循环
 *   cleanup()  可选，C++ 在停止后调用一次
 */
```

#### Step 2：重构 `VisionGProducer` 实现

`VisionGProducer::Init()`：
- 删除 C++ `Camera` 的创建逻辑（不再持有 `impl_->camera`）
- 只初始化 Python 运行时、注册 `aipc` 模块到 Python（通过 pybind11 暴露 `submit_frame`、`is_running` 等接口）

`VisionGProducer::FrameLoop()`（或更名为 `RunPythonScript()`）：
- 不再调用 C++ Camera，不再调用 `process(frame)`
- 改为调用 Python 的 `run()` 函数，让 Python 驱动一切
- Python 内部调用 `aipc.submit_frame(frame)` 将帧推入 C++ 编码队列

`VisionGProducer::Impl` 结构：
- 删除 `std::unique_ptr<Camera> camera`
- 添加编码帧推送队列或回调机制（供 `aipc.submit_frame` 使用）

`SetResolution()` / `SetFrameRate()`：
- 从 `VisionGProducer` 中删除（Python 自管分辨率，C++ 无需提供此接口）

`ProducerConfig::ai_width / ai_height`：
- 从 `ProducerConfig` 中删除（详见问题 1 方案）

#### Step 3：更新 `kDefaultVisionGScript`

将 Phase B 透传脚本替换为目标架构的透传脚本：

```python
# Default VisionG project: camera passthrough
# Python 负责摄像头采集和帧循环，通过 aipc.submit_frame() 将帧送给 C++ 编码。
#
# 契约：
#   init()      可选，初始化资源
#   run()       必须，驱动帧循环直至 aipc.is_running() 返回 False
#   cleanup()   可选，释放资源

import visiong
import aipc

_cam = None

def init():
    global _cam
    _cam = visiong.Camera(640, 360, format='rgb')
    _cam.skip(8)

def run():
    while aipc.is_running():
        frame = _cam.snapshot()
        if not frame.is_valid():
            continue
        aipc.submit_frame(frame)

def cleanup():
    global _cam
    if _cam:
        _cam.release()
        _cam = None
```

#### Step 4：更新 `DEFAULT_YOLOV5_PROJECT_CODE`（`http.cpp`）

将 Phase B 风格的 YOLOv5 示例更新为目标架构风格：

```python
# YOLOv5 目标检测工程
#
# Python 驱动架构：Python 负责摄像头、帧循环、推理；
# 通过 aipc.submit_frame() 将处理后的帧送 C++ 进行 H.264 编码和流媒体分发。
#
#   init()      可选，初始化摄像头和模型
#   run()       必须，帧循环主控
#   cleanup()   可选，释放资源

import visiong
import aipc

MODEL_PATH  = "../model/yolov5.rknn"
LABEL_PATH  = "../model/coco_80_labels_list.txt"
BOX_THRESHOLD = 0.25
NMS_THRESHOLD = 0.45

_cam      = None
_detector = None

def init():
    global _cam, _detector
    try:
        visiong.NpuClock().set_rate_mhz(420,
            update_cru_clk500m_src=True, unbind_rebind_npu=True)
    except Exception as e:
        print("[YOLOV5][WARN] NPU clock setup skipped:", e)
    _cam = visiong.Camera(640, 360, format='rgb')
    _cam.skip(8)
    _detector = visiong.NPU('yolov5', MODEL_PATH, LABEL_PATH,
                            box=BOX_THRESHOLD, nms=NMS_THRESHOLD)
    print("[YOLOV5][INFO] detector loaded:", MODEL_PATH)

def run():
    while aipc.is_running():
        frame = _cam.snapshot()
        if not frame.is_valid():
            continue
        out = frame.to_format('bgr888')
        for result in _detector.infer(frame, model_format='rgb'):
            x, y, w, h = result.box
            out.draw_rectangle(x, y, w, h, color=(0, 255, 0), thickness=2)
            out.draw_string(x, max(0, y - 20),
                            f'{result.label} {result.score:.2f}',
                            color=(0, 255, 0), scale=0.9, thickness=2)
        aipc.submit_frame(out)

def cleanup():
    global _cam, _detector
    if _cam:
        _cam.release()
        _cam = None
    _detector = None
    print("[YOLOV5][INFO] resources released")
```

#### Step 5：更新 `http.cpp` 状态端点

`/api/python/status`：

```cpp
json cam_info;
cam_info["managed_by"] = "python";   // Python 自管摄像头
// C++ 不持有摄像头参数，不再报告 width/height
data["camera"] = cam_info;
```

`/api/pipeline/status` VisionG 分支：

```cpp
// VisionG: 摄像头由 Python 脚本自行管理
data["available_resolutions"] = json::array();
data["note"] = "Camera and resolution are fully managed by the Python project script (visiong.Camera).";
// 删除原有的 cam_info width/height 字段
```

---

## 变更影响范围汇总

| 问题 | 需修改文件 | 修改类型 |
|------|-----------|---------|
| 问题 1 | `i_media_producer.h` | 删除 `Resolution`/`ResolutionConfig`/`ai_width/ai_height`/`SetResolution`/`SetFrameRate` |
| 问题 1 | `simple_ipc/simple_ipc_config.h`（新建） | 新增 SimpleIPC 专用分辨率配置 |
| 问题 1 | `media_manager.h/.cpp` | `SetResolution` 加模式守卫，改用 `dynamic_cast` |
| 问题 1 | `visiong_producer.h/.cpp` | 删除 `SetResolution/SetFrameRate`，删除 Camera 相关逻辑 |
| 问题 1 | `simple_ipc_producer.h/.cpp` | `SetResolution/SetFrameRate` 改为具体方法（非虚） |
| 问题 1 | `http.cpp` | 更新 `/api/pipeline/status`、`/api/python/status` 端点 |
| 问题 2 | `simple_ipc/mpi_config.h` | 命名空间改 `media::simple_ipc`，删除遗留函数和常量 |
| 问题 2 | `simple_ipc_producer.cpp` | 更新命名空间引用 |
| 问题 3 | `visiong_producer.h` | 重写文件注释和 Python 契约描述 |
| 问题 3 | `visiong_producer.cpp` | 更新 `kDefaultVisionGScript`，重构 Init/FrameLoop |
| 问题 3 | `http.cpp` | 更新 `DEFAULT_YOLOV5_PROJECT_CODE` 和状态端点 |

---

## 附：架构对照表

| 维度 | Phase B（当前代码） | 目标架构 |
|------|-------------------|---------|
| 摄像头所有权 | C++（`Camera` 对象） | Python（`visiong.Camera`） |
| 帧循环驱动方 | C++（`VisionGProducer::FrameLoop`） | Python（`run()` 函数内的 `while` 循环） |
| 分辨率配置 | C++（`ProducerConfig::ai_width/ai_height`） | Python（`visiong.Camera(w, h, ...)`） |
| 模型加载 | Python（`init()` 中） | Python（`init()` 中，不变） |
| 推理与绘制 | Python（`process(frame)` 中） | Python（`run()` 循环中） |
| 帧提交方式 | C++ 调用 Python `process(frame)` → 返回值 | Python 调用 `aipc.submit_frame(frame)` |
| Python 契约 | `process(frame) -> ImageBuffer \| None` | `run()`（无参，自驱循环） |
| C++ 编码 | 接收 `process()` 返回值后送 VENC | 接收 `submit_frame()` 推入的帧后送 VENC |
