我在thirdparty中添加了一个visiong库，我希望用这个库来取代media produceer中除了simple ipc之外的edge ai部分，包括rknn运行时管理、硬件资源管理分配（vi、vpss等），调用visiong库等c++接口来实现。我需要你分析一下可行性，并给出修改修改思路

# VisionG 库替代 Edge AI 可行性分析

## 1. 结论：完全可行

VisionG 库提供了 **Camera、NPU、VencManager、ImageBuffer** 四大核心 C++ 类，能够完整替代当前 [YoloProducer](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/yolov5/yolo_producer.h#79-81) 和 [RetinaFaceProducer](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/i_media_producer.h#295-307) 中的以下手动管理逻辑：

| 当前手动管理                            | VisionG 替代                                                 |
| --------------------------------------- | ------------------------------------------------------------ |
| ISP 初始化 (`SAMPLE_COMM_ISP_Init/Run`) | [Camera](file:///home/hao/projects/luckfox-pico/aipc/thirdparty/visiong/include/visiong/core/Camera.h#24-25) 内部自动管理 |
| VI 设备/通道配置 (`RK_MPI_VI_*`)        | `Camera::snapshot()` 封装                                    |
| VENC 配置 (`RK_MPI_VENC_*`)             | `VencManager::encodeToVideo()`                               |
| MB Pool 管理 (`RK_MPI_MB_*`)            | [ImageBuffer](file:///home/hao/projects/luckfox-pico/aipc/thirdparty/visiong/include/visiong/core/ImageBuffer.h#73-74) 自动管理 |
| RKNN 上下文管理                         | [NPU](file:///home/hao/projects/luckfox-pico/aipc/thirdparty/visiong/include/visiong/npu/NPU.h#48-49) 类封装 |
| NV12→BGR 转换 + letterbox               | `ImageBuffer::to_format()` / `ImageBuffer::letterbox()`      |
| OpenCV 画框 / 文字                      | `ImageBuffer::draw_rectangle()` / `draw_string()`            |
| MPI 系统初始化/退出                     | VisionG 内部自动管理                                         |

> [!IMPORTANT]
> SimpleIPC 模式 **保持不变**，因为它使用 VI→VPSS→VENC 的硬件绑定零拷贝管线，VisionG 的 snapshot 模式不适合此场景。

---

## 2. 当前架构 vs 替换后架构

### 当前 YoloProducer 数据流（~576 行代码）

```
ISP Init → MPI SYS Init → VI Dev/Chn Init → VENC Init → MB Pool Create
  ↓
FrameLoop:
  RK_MPI_VI_GetChnFrame → NV12 (raw MPI)
    → cv::cvtColor (NV12→BGR)
    → cv::resize + letterbox
    → memcpy to RKNN input → rknn_run → post-process
    → cv::rectangle / putText (OSD)
    → memcpy to VENC buffer
    → RK_MPI_VENC_SendFrame → RK_MPI_VENC_GetStream
    → Dispatch H264 stream
```

### 替换后 VisionGProducer 数据流（预估 ~200 行代码）

```
Camera(width, height, "yuv") → NPU(ModelType::YOLOV5, model_path)
  ↓
FrameLoop:
  Camera::snapshot() → ImageBuffer (NV12, zero-copy)
    → NPU::inference(img_buf) → vector<Detection>
    → img_buf.draw_rectangle() / draw_string() (OSD)
    → VencManager::encodeToVideo(img_buf) → VencEncodedPacket
    → 转换为 EncodedStreamPtr → Dispatch
```

---

## 3. API 映射详解

### 3.1 摄像头采集（Camera 替代 ISP + VI + MPI SYS）

```cpp
// 当前：~60 行 MPI 初始化代码
SAMPLE_COMM_ISP_Init(kViDev, hdr_mode, multi_sensor, iq_dir);
SAMPLE_COMM_ISP_Run(kViDev);
RK_MPI_SYS_Init();
vi_dev_init();
vi_chn_init(kViChn, width, height);
// ...取帧
RK_MPI_VI_GetChnFrame(kViDev, kViChn, &stViFrame, 1000);

// VisionG 替代：~3 行
Camera cam(width, height, "yuv");
// ...取帧
ImageBuffer frame = cam.snapshot();
```

### 3.2 NPU 推理（NPU 替代 RKNN 手动管理）

```cpp
// 当前：~50 行 RKNN 初始化 + 推理 + 后处理
auto ai_model = std::make_unique<rknn::YoloV5Model>();
ai_model->Init(model_cfg);
// letterbox + memcpy + Run + GetResults...

// VisionG 替代：~3 行
NPU npu(ModelType::YOLOV5, model_path, label_path, 0.25f, 0.45f);
auto detections = npu.inference(frame);  // 内部自动处理 letterbox/格式转换
```

### 3.3 OSD 绘制（ImageBuffer 替代 OpenCV）

```cpp
// 当前：OpenCV + 手动坐标映射
cv::rectangle(bgr, cv::Point(sX, sY), cv::Point(eX, eY), ...);
cv::putText(bgr, text, cv::Point(sX, sY-8), ...);

// VisionG 替代：ImageBuffer 内置绘制
auto& [x, y, w, h] = det.box;
frame.draw_rectangle(x, y, w, h, {0, 255, 0}, 3, false);
frame.draw_string(x, y-8, det.label, {0, 255, 0}, 1.0, 2);
```

### 3.4 视频编码（VencManager 替代 VENC MPI）

```cpp
// 当前：~40 行 VENC 初始化 + 手动 SendFrame/GetStream
venc_init(kVencChn, width, height, RK_VIDEO_ID_AVC);
RK_MPI_VENC_SendFrame(kVencChn, &h264_frame, -1);
auto stream = acquire_encoded_stream(kVencChn, 1000, &last_error);

// VisionG 替代：~3 行
auto& venc = VencManager::getInstance();
VencEncodedPacket packet;
venc.encodeToVideo(frame, VencCodec::H264, 75, packet);
```

---

## 4. 需要解决的关键问题

### 4.1 编码流格式适配（核心难点）

当前系统的消费者回调使用 `EncodedStreamPtr`（自定义类型），而 VisionG 输出 [VencEncodedPacket](file:///home/hao/projects/luckfox-pico/aipc/thirdparty/visiong/include/visiong/modules/VencManager.h#27-36)。需要写一个转换层：

```cpp
// VencEncodedPacket → EncodedStreamPtr 转换
EncodedStreamPtr ConvertToStream(const VencEncodedPacket& packet) {
    auto stream = std::make_shared<EncodedStream>();
    stream->data = packet.data;          // std::vector<unsigned char>
    stream->is_keyframe = packet.is_keyframe;
    // ...填充其他字段
    return stream;
}
```

### 4.2 Camera 与 SimpleIPC 的 ISP/VI 冲突

> [!WARNING]
> VisionG 的 [Camera](file:///home/hao/projects/luckfox-pico/aipc/thirdparty/visiong/include/visiong/core/Camera.h#24-25) 内部会自行初始化 ISP 和 VI。当从 SimpleIPC 切换到 AI 模式时，需要确保 SimpleIPC 的 ISP/VI 已经完全释放，否则会产生硬件资源冲突。

当前的 [force_cleanup_mpi_state()](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/yolov5/mpi_config.h#48-83) 机制已经处理了这个问题。VisionG 的 Camera 在构造时会重新初始化，所以冷切换流程不变。

### 4.3 VisionG 的 VencManager 是单例

VisionG 的 `VencManager::getInstance()` 是全局单例，而 SimpleIPC 模式有自己的 VENC 管理。模式切换时需要注意：

- 进入 AI 模式前：SimpleIPC 释放 VENC
- 退出 AI 模式时：调用 `VencManager::releaseVencIfUnused()` 释放

### 4.4 NPU 检测结果坐标系

VisionG `NPU::inference()` 返回的 `Detection::box` 是 `std::tuple<int,int,int,int>`，其坐标是否已经映射回原图尺寸需要确认。根据 VisionG 源码结构，推理时传入 [ImageBuffer](file:///home/hao/projects/luckfox-pico/aipc/thirdparty/visiong/include/visiong/core/ImageBuffer.h#73-74)，内部应该会自动处理 letterbox 和坐标反映射。

---

## 5. 修改思路

### 第一步：创建 VisionGProducer 类

新建 `src/media_producer/visiong/` 目录，创建 `visiong_producer.h/cpp`，实现 [IMediaProducer](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/i_media_producer.h#258-259) 接口：

```
src/media_producer/visiong/
├── CMakeLists.txt
├── visiong_producer.h
└── visiong_producer.cpp
```

核心成员：

```cpp
class VisionGProducer : public IMediaProducer {
private:
    std::unique_ptr<Camera> camera_;
    std::unique_ptr<NPU> npu_;
    SerialStreamDispatcher dispatcher_;
    std::thread frame_thread_;
    // ...
};
```

### 第二步：实现生命周期

```cpp
int VisionGProducer::Init() {
    camera_ = std::make_unique<Camera>(width, height, "yuv");
    npu_ = std::make_unique<NPU>(model_type, model_path, label_path);
    return 0;
}

int VisionGProducer::Deinit() {
    npu_.reset();
    camera_->release();
    camera_.reset();
    VencManager::getInstance().releaseVencIfUnused();
    return 0;
}
```

### 第三步：实现帧处理循环

```cpp
void VisionGProducer::FrameLoop() {
    auto& venc = VencManager::getInstance();
    VencManager::ScopedUser venc_user(venc);

    while (running_) {
        // 1. 采集
        ImageBuffer frame = camera_->snapshot();

        // 2. 推理
        auto detections = npu_->inference(frame);

        // 3. OSD 绘制
        for (auto& det : detections) {
            auto [x, y, w, h] = det.box;
            frame.draw_rectangle(x, y, w, h, {0,255,0}, 2, false);
            frame.draw_string(x, y-8, det.label, {0,255,0}, 1.0, 2);
        }

        // 4. 编码
        VencEncodedPacket packet;
        if (venc.encodeToVideo(frame, VencCodec::H264, 75, packet)) {
            auto stream = ConvertToEncodedStream(packet);
            dispatcher_.DispatchFrame(stream);
        }
    }
}
```

### 第四步：更新工厂和枚举

修改 [i_media_producer.h](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/i_media_producer.h)，添加 `ProducerMode::VisionG_YoloV5` 等模式；修改 [media_manager.cpp](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/media_manager.cpp) 的工厂逻辑。

### 第五步：清理旧代码（可选/渐进式）

暂时保留 `yolov5/` 和 `retainface/` 目录，通过 CMake 选项控制编译。验证 VisionG 方案稳定后再移除。

---

## 6. 可删除的代码量

| 文件                                                         | 行数        | 说明                              |
| ------------------------------------------------------------ | ----------- | --------------------------------- |
| [yolov5/yolo_producer.cpp](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/yolov5/yolo_producer.cpp) | 576         | 整个文件可替代                    |
| [yolov5/yolov5_model.cpp](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/yolov5/yolov5_model.cpp) | ~700        | RKNN 模型管理，NPU 类替代         |
| [yolov5/yolov5_model.h](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/yolov5/yolov5_model.h) | 123         | 同上                              |
| [yolov5/mpi_config.h](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/yolov5/mpi_config.h) | 207         | MPI 配置，Camera/VencManager 替代 |
| [retainface/retinaface_producer.cpp](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/retainface/retinaface_producer.cpp) | ~500        | 同 yolo，NPU 支持 RetinaFace      |
| [retainface/retinaface_model.cpp](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/retainface/retinaface_model.cpp) | ~600        | RKNN 管理，NPU 类替代             |
| [retainface/mpi_config.h](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/retainface/mpi_config.h) | ~200        | 同 yolov5                         |
| [common/image_utils.cpp](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/common/image_utils.cpp) | ~240        | 部分被 ImageBuffer 替代           |
| [common/osd_overlay.cpp](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/common/osd_overlay.cpp) | ~240        | 被 ImageBuffer::draw_* 替代       |
| **合计**                                                     | **~3400行** | 替换为 ~200 行 VisionGProducer    |

---

## 7. 风险与注意事项

| 风险                                                         | 等级 | 应对                 |
| ------------------------------------------------------------ | ---- | -------------------- |
| VisionG Camera snapshot 逐帧取帧，可能比 VI GetChnFrame 延迟更高 | 🟡 中 | 实测对比帧率和延迟   |
| VencManager 单例与 SimpleIPC 的 VENC 共存                    | 🟡 中 | 冷切换时确保互斥     |
| VisionG 编译依赖（需要 visiong-stage 目录）                  | 🟢 低 | 已有 build.sh 脚本   |
| NPU inference 坐标映射是否正确                               | 🟡 中 | 需实测确认           |
| ImageBuffer 的 draw_* 性能（是否用 RGA 硬件加速）            | 🟢 低 | VisionG 内部使用 RGA |