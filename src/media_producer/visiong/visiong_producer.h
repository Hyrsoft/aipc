/**
 * @file visiong_producer.h
 * @brief VisionG 库驱动的 AI 推理模式生产者
 *
 * 使用 VisionG 库管理 Camera 取帧和 VencManager 编码，
 * 具体的 AI 模型推理和 OSD 绘制逻辑委托给 IModelStrategy。
 *
 * 数据流：
 *   Camera::snapshot() → ImageBuffer (NV12)
 *     → IModelStrategy::ProcessFrame() (推理 + OSD)
 *     → VencManager::encodeToVideo() → H264 编码流
 */

#pragma once

#include "../i_media_producer.h"
#include "i_model_strategy.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

// 前向声明 VisionG 类型
class Camera;
class NPU;

namespace media {

/**
 * @class VisionGProducer
 * @brief VisionG 库驱动的 AI 推理模式生产者
 *
 * 通过 IModelStrategy 支持不同 AI 模型（YOLOv5、RetinaFace、YOLO11 等）。
 */
class VisionGProducer : public IMediaProducer {
public:
    /**
     * @brief 构造函数
     * @param config 配置参数
     * @param strategy 模型策略（决定使用哪个 AI 模型及 OSD 逻辑）
     */
    VisionGProducer(const ProducerConfig& config,
                    std::unique_ptr<IModelStrategy> strategy);

    ~VisionGProducer() override;

    // ========== IMediaProducer 接口实现 ==========

    int Init() override;
    int Deinit() override;
    bool Start() override;
    void Stop() override;

    void RegisterStreamConsumer(const std::string& name, StreamCallback callback,
                                StreamConsumerType type = StreamConsumerType::AsyncIO,
                                int queue_size = 3) override;

    void ClearStreamConsumers() override;

    bool IsInitialized() const override { return initialized_.load(); }
    bool IsRunning() const override { return running_.load(); }
    const char* GetTypeName() const override { return type_name_.c_str(); }
    const ProducerConfig& GetConfig() const override { return config_; }

    int SetResolution(Resolution preset) override;
    int SetFrameRate(int fps) override;

    /**
     * @brief 获取内部模型策略指针（用于 Python 编辑器 API 直接操作）
     */
    IModelStrategy* GetStrategy() { return strategy_.get(); }

    /**
     * @brief 动态替换 NPU 实例（用于模型切换），需在帧循环暂停时调用
     * @param new_npu 新的 NPU 实例
     */
    void ReplaceNPU(std::unique_ptr<NPU> new_npu);

    /**
     * @brief 暂停/恢复帧循环（用于 NPU 模型切换期间）
     */
    void PauseFrameLoop();
    void ResumeFrameLoop();

private:
    // 禁止拷贝
    VisionGProducer(const VisionGProducer&) = delete;
    VisionGProducer& operator=(const VisionGProducer&) = delete;

    /**
     * @brief 帧处理主循环
     */
    void FrameLoop();

private:
    ProducerConfig config_;
    std::string type_name_;

    // VisionG 组件
    std::unique_ptr<IModelStrategy> strategy_;
    std::unique_ptr<Camera> camera_;
    std::unique_ptr<NPU> npu_;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};

    std::thread frame_thread_;

    // 内部实现
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // 统计
    std::atomic<uint64_t> frame_count_{0};
    std::atomic<uint64_t> inference_count_{0};
};

} // namespace media
