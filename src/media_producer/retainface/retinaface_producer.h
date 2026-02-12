/**
 * @file retinaface_producer.h
 * @brief RetinaFace 人脸检测模式生产者
 *
 * 数据流架构与 YoloProducer 类似，但使用 RetinaFace 模型进行人脸检测。
 *
 * 特点：
 * - VPSS 和 VENC 解绑，由软件控制时序
 * - 支持 RetinaFace 人脸检测 + 5 点关键点
 * - 检测框和关键点通过 OSD 叠加到编码流
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#pragma once

#include "../i_media_producer.h"

#include <memory>
#include <atomic>
#include <mutex>
#include <thread>

namespace media {

/**
 * @class RetinaFaceProducer
 * @brief RetinaFace 人脸检测模式生产者
 *
 * 实现串行帧处理流水线，支持人脸检测和关键点标注。
 */
class RetinaFaceProducer : public IMediaProducer {
public:
    /**
     * @brief 构造函数
     * @param config 配置参数
     */
    explicit RetinaFaceProducer(const ProducerConfig& config);
    
    ~RetinaFaceProducer() override;

    // ========== IMediaProducer 接口实现 ==========

    int Init() override;
    int Deinit() override;
    bool Start() override;
    void Stop() override;

    void RegisterStreamConsumer(
        const std::string& name,
        StreamCallback callback,
        StreamConsumerType type = StreamConsumerType::AsyncIO,
        int queue_size = 3) override;

    void ClearStreamConsumers() override;

    bool IsInitialized() const override { return initialized_.load(); }
    bool IsRunning() const override { return running_.load(); }
    const char* GetTypeName() const override { return "RetinaFace"; }
    const ProducerConfig& GetConfig() const override { return config_; }

    int SetResolution(Resolution preset) override;
    int SetFrameRate(int fps) override;

private:
    RetinaFaceProducer(const RetinaFaceProducer&) = delete;
    RetinaFaceProducer& operator=(const RetinaFaceProducer&) = delete;

    int InitMpi();
    int DeinitMpi();
    int InitAiEngine();
    void DeinitAiEngine();
    bool InitRgbPool();
    void FrameLoop();
    bool ProcessFrame(void* nv12_data, int width, int height, int stride,
                      void* rgb_output, uint64_t pts);

private:
    ProducerConfig config_;
    
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    
    std::thread frame_thread_;
    
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<uint64_t> frame_count_{0};
    std::atomic<uint64_t> inference_count_{0};
};

}  // namespace media
