# RKNN 模块 - AI 推理引擎

## 概述

本模块提供基于 RV1106 NPU (RKNN) 的 AI 推理能力，采用**策略模式**实现动态模型切换。支持：

- **YOLOv5** - 80 类目标检测（COCO 数据集）
- **RetinaFace** - 人脸检测 + 5 点关键点

## 架构设计

```
┌─────────────────────────────────────────────────────────────┐
│                      AIEngine (单例)                        │
│  - 线程安全的模型管理                                        │
│  - 统一的推理接口                                            │
│  - VPSS 重配置回调                                           │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                     AIModelBase (抽象基类)                   │
│  - Init() / Deinit()                                        │
│  - SetInput() / Run() / GetResults()                        │
│  - GetInputSize() / GetModelInfo()                          │
└─────────────────────────────────────────────────────────────┘
              ┌───────────────┴───────────────┐
              ▼                               ▼
┌─────────────────────────┐     ┌─────────────────────────┐
│     YoloV5Model         │     │   RetinaFaceModel       │
│  - 640x640 输入         │     │  - 640x640 / 320x320    │
│  - 3 输出层解码         │     │  - 人脸框 + 5 关键点    │
│  - COCO 80 类           │     │  - Prior Box 解码       │
└─────────────────────────┘     └─────────────────────────┘
```

## 核心特性

### 1. 冷切换策略 (Cold Switch)

RV1106 内存有限（128MB/256MB），不允许同时加载多个模型。切换流程：

```
Stop Inference → Unload Old Model → Load New Model → Start Inference
```

### 2. 零拷贝内存 (Zero-Copy)

使用 `rknn_create_mem()` 分配 DMA 内存，VPSS 输出可直接写入，避免 CPU 拷贝。

### 3. 线程安全

`AIEngine` 使用互斥锁保护模型指针，支持：
- HTTP 线程发起模型切换
- AI 线程执行推理

## 使用示例

### 基本用法

```cpp
#include "rknn/ai_engine.h"

using namespace rknn;

// 获取引擎实例
auto& engine = AIEngine::Instance();

// 加载 YOLOv5 模型
ModelConfig config;
config.model_path = "./model/yolov5.rknn";
config.labels_path = "./model/coco_80_labels_list.txt";
config.conf_threshold = 0.25f;
config.nms_threshold = 0.45f;

int ret = engine.SwitchModel(ModelType::kYoloV5, config);
if (ret != 0) {
    // 处理错误
}

// 推理（拷贝模式）
DetectionResultList results;
engine.ProcessFrame(rgb_data, 640, 640, results);

// 打印结果
for (const auto& det : results.results) {
    printf("[%s] conf=%.2f box=(%d,%d,%d,%d)\n",
           det.label.c_str(), det.confidence,
           det.box.x, det.box.y, det.box.width, det.box.height);
}
```

### 零拷贝模式

```cpp
// 获取输入内存地址
void* input_addr = nullptr;
int input_size = 0;
engine.GetInputMemory(&input_addr, &input_size);

// 直接从 VPSS 输出拷贝到输入内存
// 或者配置 VPSS 输出地址为 input_addr（更高级）
memcpy(input_addr, vpss_frame_data, input_size);

// 执行推理
DetectionResultList results;
engine.RunInference(results);
```

### HTTP 动态切换

```cpp
// 在 HTTP 处理线程中
server.Post("/api/ai/switch", [&](const Request& req, Response& res) {
    auto model_name = parse_json(req.body)["model"];
    
    ModelConfig config;
    if (model_name == "yolov5") {
        config.model_path = "./model/yolov5.rknn";
        config.labels_path = "./model/coco_80_labels_list.txt";
        engine.SwitchModel(ModelType::kYoloV5, config);
    } else if (model_name == "retinaface") {
        config.model_path = "./model/retinaface.rknn";
        engine.SwitchModel(ModelType::kRetinaFace, config);
    }
    
    res.set_content("OK", "text/plain");
});
```

### VPSS 联动

```cpp
// 设置 VPSS 重配置回调
engine.SetVpssReconfigureCallback([](int width, int height) {
    // 当模型输入尺寸变化时调用
    // 重配置 VPSS Chn1 的输出分辨率
    VPSS_CHN_ATTR_S attr;
    attr.enChnMode = VPSS_CHN_MODE_USER;
    attr.u32Width = width;
    attr.u32Height = height;
    // ...
    RK_MPI_VPSS_SetChnAttr(0, 1, &attr);
    return 0;
});
```

## 统一结果结构

```cpp
struct DetectionResult {
    int class_id;           // 类别 ID
    float confidence;       // 置信度 [0.0, 1.0]
    BoundingBox box;        // 边界框 {x, y, width, height}
    std::string label;      // 类别名称
    std::vector<Landmark> landmarks;  // 关键点（RetinaFace 用）
};
```

## 文件结构

```
src/rknn/
├── CMakeLists.txt       # 构建配置
├── README.md            # 本文档
├── ai_types.h           # 类型定义
├── ai_model_base.h      # 抽象基类
├── ai_engine.h          # 引擎头文件
├── ai_engine.cpp        # 引擎实现
├── yolov5_model.h       # YOLOv5 头文件
├── yolov5_model.cpp     # YOLOv5 实现
├── retinaface_model.h   # RetinaFace 头文件
└── retinaface_model.cpp # RetinaFace 实现
```

## 依赖

- `librknnmrt.so` - RKNN 运行时库
- `rknn_api.h` - RKNN API 头文件
- `rknn_box_priors.h` - RetinaFace Prior Box 数据

## 性能注意事项

1. **推理延迟**：YOLOv5 640x640 约 80-100ms，RetinaFace 约 50-70ms
2. **内存占用**：单模型约 10-30MB NPU 内存
3. **CPU 占用**：后处理（NMS）约 5-10% CPU

## 后续扩展

- [ ] RGA 硬件缩放/色彩转换
- [ ] 多模型级联（检测 + 识别）
- [ ] 异步推理模式
- [ ] 模型性能统计
