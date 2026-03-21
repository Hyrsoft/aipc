/**
 * @file visiong_producer.h
 * @brief VisionG Python 模式生产者
 *
 * VisionG 模式下，Python 全权负责视觉逻辑（采集/推理/绘制），
 * C++ 仅负责：Python 代码管理、生命周期管理、编码和分发。
 */

#pragma once

#include "../i_media_producer.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class ImageBuffer;

namespace media {

// 预热内嵌 Python 解释器与 visiong 模块，建议在主线程启动早期调用。
void WarmupVisionGPythonRuntime();

/**
 * @class VisionGProducer
 * @brief Python 驱动的 VisionG 生产者
 */
class VisionGProducer : public IMediaProducer {
public:
    explicit VisionGProducer(const ProducerConfig& config);

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

    std::string GetCurrentCode() const;
    std::string GetLastError() const;
    std::string UpdateCode(const std::string& code);

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

    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};

    std::thread frame_thread_;

    // 内部实现
    struct Impl;
    std::unique_ptr<Impl> impl_;

    mutable std::mutex state_mutex_;
    std::string current_code_;
    std::string last_error_;

    // 统计
    std::atomic<uint64_t> frame_count_{0};
    std::atomic<uint64_t> encode_count_{0};
};

} // namespace media
