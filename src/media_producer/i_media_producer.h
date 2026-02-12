/**
 * @file i_media_producer.h
 * @brief 媒体生产者接口 - 定义统一的视频采集与编码接口
 *
 * 设计模式：策略模式 (Strategy Pattern)
 * 
 * 核心设计思想：
 * - 外部接口统一，内部硬件资源分配逻辑完全独立且差异巨大
 * - 每个子类是一个独立的"黑盒"，内部包含各自的 VI/VPSS/VENC 配置
 * - 运行时通过多态 (vtable) 动态分发到具体实现
 *
 * 子类实现：
 * - SimpleIPCProducer: 纯监控模式（VI -> VPSS -> VENC 硬件绑定，零拷贝）
 * - YoloProducer: YOLOv5 AI 推理模式（手动帧控制 + NPU 推理 + OSD）
 * - RetinaFaceProducer: RetinaFace 人脸检测模式
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#pragma once

#include "common/media_buffer.h"

#include <functional>
#include <memory>
#include <string>

namespace media {

// ============================================================================
// 流消费者类型
// ============================================================================

/**
 * @brief 流消费者类型
 */
enum class StreamConsumerType {
    Direct,     ///< 直接在 Fetch 线程中执行（仅用于极快的操作）
    AsyncIO,    ///< 通过 asio::post 投递到 IO 线程执行（网络发送）
    Queued      ///< 通过队列投递到独立线程（文件写入等阻塞操作）
};

/**
 * @brief 编码流回调类型
 */
using StreamCallback = std::function<void(EncodedStreamPtr)>;

// ============================================================================
// 分辨率配置
// ============================================================================

/**
 * @brief 分辨率预设
 */
enum class Resolution {
    R_1080P,    ///< 1920x1080 @ 30fps (IPC 默认)
    R_720P,     ///< 1280x720 @ 30fps
    R_480P,     ///< 720x480 @ 30fps (推理模式推荐)
};

/**
 * @brief 分辨率配置
 */
struct ResolutionConfig {
    int width = 1920;
    int height = 1080;
    int framerate = 30;
    
    static ResolutionConfig FromPreset(Resolution preset) {
        switch (preset) {
            case Resolution::R_1080P: return {1920, 1080, 30};
            case Resolution::R_720P:  return {1280, 720, 30};
            case Resolution::R_480P:  return {720, 480, 30};
            default:                  return {1920, 1080, 30};
        }
    }
};

// ============================================================================
// 生产者配置
// ============================================================================

/**
 * @brief 媒体生产者通用配置
 */
struct ProducerConfig {
    Resolution resolution = Resolution::R_1080P;
    int framerate = 30;
    int bitrate_kbps = 10 * 1024;  // 10 Mbps
    
    // AI 相关（仅对 AI 模式有效）
    int ai_width = 640;
    int ai_height = 640;
    
    /**
     * @brief 获取分辨率配置
     */
    ResolutionConfig GetResolutionConfig() const {
        auto cfg = ResolutionConfig::FromPreset(resolution);
        cfg.framerate = framerate;
        return cfg;
    }
};

// ============================================================================
// 媒体生产者接口
// ============================================================================

/**
 * @class IMediaProducer
 * @brief 媒体生产者纯虚基类（接口）
 *
 * 定义视频采集与编码的统一接口，具体实现由子类完成。
 * 
 * 生命周期：
 * 1. 构造函数 - 创建实例（不分配硬件资源）
 * 2. Init() - 初始化硬件资源（VI/VPSS/VENC）
 * 3. RegisterStreamConsumer() - 注册编码流消费者
 * 4. Start() - 启动视频流
 * 5. Stop() - 停止视频流
 * 6. Deinit() - 释放硬件资源
 * 7. 析构函数 - 销毁实例
 *
 * 使用方式：
 * @code
 * // 通过工厂方法创建具体实现
 * auto producer = CreateSimpleIPCProducer(config);
 * 
 * // 初始化
 * if (producer->Init() != 0) {
 *     // 处理错误
 * }
 * 
 * // 注册消费者
 * producer->RegisterStreamConsumer("rtsp", rtsp_callback);
 * 
 * // 启动
 * producer->Start();
 * 
 * // ... 运行中 ...
 * 
 * // 停止并销毁
 * producer->Stop();
 * producer->Deinit();
 * // producer 被销毁时自动释放资源
 * @endcode
 */
class IMediaProducer {
public:
    virtual ~IMediaProducer() = default;

    // ========== 生命周期管理 ==========

    /**
     * @brief 初始化媒体生产者
     * 
     * 分配硬件资源（ISP/VI/VPSS/VENC），建立视频采集与编码链路。
     * 
     * @return 0 成功，-1 失败
     * 
     * @note 每个子类有独立的硬件配置逻辑
     */
    virtual int Init() = 0;

    /**
     * @brief 反初始化媒体生产者
     * 
     * 释放所有硬件资源，停止视频采集与编码。
     * 
     * @return 0 成功
     */
    virtual int Deinit() = 0;

    /**
     * @brief 启动视频流
     * 
     * 开始视频采集、编码和分发。
     * 
     * @return true 成功，false 失败
     */
    virtual bool Start() = 0;

    /**
     * @brief 停止视频流
     * 
     * 停止视频采集和分发，但不释放硬件资源。
     */
    virtual void Stop() = 0;

    // ========== 流消费者接口 ==========

    /**
     * @brief 注册编码流消费者
     * 
     * @param name 消费者名称（用于日志）
     * @param callback 回调函数
     * @param type 消费者类型
     * @param queue_size 队列大小（仅对 Queued 类型有效）
     */
    virtual void RegisterStreamConsumer(
        const std::string& name,
        StreamCallback callback,
        StreamConsumerType type = StreamConsumerType::AsyncIO,
        int queue_size = 3) = 0;

    /**
     * @brief 清除所有流消费者
     */
    virtual void ClearStreamConsumers() = 0;

    // ========== 状态查询 ==========

    /**
     * @brief 检查是否已初始化
     */
    virtual bool IsInitialized() const = 0;

    /**
     * @brief 检查是否正在运行
     */
    virtual bool IsRunning() const = 0;

    /**
     * @brief 获取生产者类型名称
     */
    virtual const char* GetTypeName() const = 0;

    /**
     * @brief 获取当前配置
     */
    virtual const ProducerConfig& GetConfig() const = 0;

    // ========== 可选配置接口 ==========

    /**
     * @brief 设置分辨率
     * 
     * @param preset 分辨率预设
     * @return 0 成功，-1 失败
     * 
     * @note 需要在 Init() 之前调用，或者在 Stop() 后重新 Init()
     * @note 默认实现返回 -1（不支持）
     */
    virtual int SetResolution(Resolution preset) { (void)preset; return -1; }

    /**
     * @brief 设置帧率
     * 
     * @param fps 帧率（1-30）
     * @return 0 成功，-1 失败
     * 
     * @note 默认实现返回 -1（不支持）
     */
    virtual int SetFrameRate(int fps) { (void)fps; return -1; }

protected:
    IMediaProducer() = default;
    
    // 禁止拷贝
    IMediaProducer(const IMediaProducer&) = delete;
    IMediaProducer& operator=(const IMediaProducer&) = delete;
};

// ============================================================================
// 工厂函数
// ============================================================================

/**
 * @brief 创建纯 IPC 模式生产者
 * 
 * 特点：
 * - VI -> VPSS -> VENC 硬件绑定，零拷贝
 * - CPU 不参与数据流
 * - 最高性能，最低延迟
 * 
 * @param config 配置参数
 * @return 生产者实例
 */
std::unique_ptr<IMediaProducer> CreateSimpleIPCProducer(const ProducerConfig& config);

/**
 * @brief 创建 YOLOv5 AI 推理模式生产者
 * 
 * 特点：
 * - VPSS -> 手动获取 -> NPU 推理 -> OSD 叠加 -> VENC
 * - 支持目标检测和框标注
 * - DDR 带宽受限，推荐 480p
 * 
 * @param config 配置参数
 * @return 生产者实例
 */
std::unique_ptr<IMediaProducer> CreateYoloProducer(const ProducerConfig& config);

/**
 * @brief 创建 RetinaFace 人脸检测模式生产者
 * 
 * 特点：
 * - VPSS -> 手动获取 -> NPU 推理 -> OSD 叠加 -> VENC
 * - 专用于人脸检测
 * - DDR 带宽受限，推荐 480p
 * 
 * @param config 配置参数
 * @return 生产者实例
 */
std::unique_ptr<IMediaProducer> CreateRetinaFaceProducer(const ProducerConfig& config);

// ============================================================================
// 生产者模式枚举
// ============================================================================

/**
 * @brief 生产者模式枚举
 */
enum class ProducerMode {
    SimpleIPC,      ///< 纯监控模式
    YoloV5,         ///< YOLOv5 目标检测
    RetinaFace      ///< RetinaFace 人脸检测
};

/**
 * @brief 通过模式枚举创建生产者
 */
std::unique_ptr<IMediaProducer> CreateProducer(ProducerMode mode, const ProducerConfig& config);

/**
 * @brief 模式枚举转字符串
 */
inline const char* ProducerModeToString(ProducerMode mode) {
    switch (mode) {
        case ProducerMode::SimpleIPC:   return "SimpleIPC";
        case ProducerMode::YoloV5:      return "YoloV5";
        case ProducerMode::RetinaFace:  return "RetinaFace";
        default:                        return "Unknown";
    }
}

}  // namespace media
