# Media Producer 媒体生产者模块

## 架构设计

### 核心设计模式：策略模式 (Strategy Pattern)

外部接口统一，内部硬件资源分配逻辑完全独立且差异巨大。

**实现方式**：继承 (Inheritance)
- 定义纯虚基类 `IMediaProducer` 作为接口
- 三种模式分别作为具体子类：
  - `SimpleIPCProducer` - 纯监控模式
  - `YoloProducer` - YOLOv5 目标检测模式
  - `RetinaFaceProducer` - RetinaFace 人脸检测模式

### 分发机制：运行时动态分发 (Dynamic Dispatch)

使用 C++ 多态 (Polymorphism) 特性：
- 编译时不确定运行的是哪种模式
- 运行时根据配置动态实例化具体子类
- 通过虚函数表 (vtable) 动态跳转到对应实现

### MediaManager：生命周期管理者

`MediaManager` 充当系统的上下文 (Context) 和控制器 (Controller)，负责"冷切换"：

1. **持有句柄**：`unique_ptr<IMediaProducer>` 基类指针
2. **接收指令**：Web 端"切换到 YOLO 模式"请求
3. **销毁旧实例**：调用 `stop()` → 重置指针 → 旧析构函数释放硬件
4. **创建新实例**：`new` 对应子类，申请全新硬件资源
5. **重新连线**：将流消费者回调注册到新实例

## 目录结构

```
media_producer/
├── i_media_producer.h      # 接口基类定义
├── media_manager.h/cpp     # 生命周期管理器
├── simple_ipc/             # 纯监控模式
│   ├── simple_ipc_producer.h/cpp
│   └── mpi_config.h        # MPI 配置工具函数
├── yolov5/                 # YOLOv5 目标检测模式
│   └── yolo_producer.h/cpp
├── retainface/             # RetinaFace 人脸检测模式
│   └── retinaface_producer.h/cpp
├── rknn/                   # AI 推理引擎（共用）
│   ├── yolov5_model.h/cpp
│   ├── retinaface_model.h/cpp
│   └── image_utils.h/cpp
└── rkvideo/                # MPI 封装（遗留，待重构）
```

## 统一接口

```cpp
class IMediaProducer {
public:
    virtual int Init() = 0;              // 初始化硬件资源
    virtual int Deinit() = 0;            // 释放硬件资源
    virtual bool Start() = 0;            // 启动视频流
    virtual void Stop() = 0;             // 停止视频流
    
    virtual void RegisterStreamConsumer(
        const std::string& name,
        StreamCallback callback,
        StreamConsumerType type = StreamConsumerType::AsyncIO,
        int queue_size = 3) = 0;
    
    virtual bool IsInitialized() const = 0;
    virtual bool IsRunning() const = 0;
    virtual const char* GetTypeName() const = 0;
};
```

## 使用示例

```cpp
#include "media_producer/media_manager.h"

using namespace media;

// 获取管理器单例
auto& mgr = MediaManager::Instance();

// 初始化（纯 IPC 模式）
ProducerConfig config;
config.resolution = Resolution::R_1080P;
mgr.Init(ProducerMode::SimpleIPC, config);

// 注册流消费者
mgr.RegisterStreamConsumer("rtsp", rtsp_callback);
mgr.RegisterStreamConsumer("webrtc", webrtc_callback);

// 启动
mgr.Start();

// 切换到 YOLOv5 模式（冷切换）
mgr.SwitchMode(ProducerMode::YoloV5);

// 切回纯 IPC 模式
mgr.SwitchMode(ProducerMode::SimpleIPC);

// 停止
mgr.Stop();
mgr.Deinit();
```

## 三种模式的硬件架构

### SimpleIPC（纯监控）

```
VI -> VPSS -> VENC (硬件绑定，零拷贝)
```
- 完全硬件绑定
- CPU 不参与数据流
- 最高性能

### YoloV5 / RetinaFace（AI 推理）

```
VI -> VPSS --(手动获取)--> NPU 推理 --(手动发送)--> VENC
         |                    |
         v                    v
      NV12 帧             RGB + OSD
```
- VPSS-VENC 解绑
- 软件控制帧流动
- 支持 OSD 叠加检测结果

## 黑盒交付原则

每个子类独立维护自己的硬件配置代码：
- VI/VPSS/VENC 配置
- 数据流转方式
- 图像格式转换
- 推理结果标注

外界只需调用统一接口，无需了解内部实现。

## 参考

- [毕设日志（四）NPU 推理与硬件资源分配](https://hyrsoft.github.io/posts/linux/毕业设计智能网络摄像头/npu推理与硬件资源分配/)

