/**
 * @file yolo_producer.h
 * @brief YOLOv5 AI 推理模式生产者
 *
 * 数据流架构：
 *   VI --> VPSS (绑定)
 *           |
 *           v (手动获取)
 *        NV12 帧
 *           |
 *           v (转换 + 缩放)
 *        RGB 帧 --> NPU 推理 --> 检测结果
 *           |
 *           v (OSD 叠加)
 *        RGB 帧 (带框)
 *           |
 *           v (手动送入)
 *        VENC --> 编码流
 *
 * 特点：
 * - VPSS 和 VENC 解绑，由软件控制时序
 * - 支持 YOLOv5 目标检测
 * - 检测框通过 OSD 叠加到编码流
 * - DDR 带宽受限，推荐 480p
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
#include <functional>
#include <vector>

namespace media {

/**
 * @class YoloProducer
 * @brief YOLOv5 AI 推理模式生产者
 *
 * 实现串行帧处理流水线，支持 AI 推理和 OSD 叠加。
 */
class YoloProducer : public IMediaProducer {
public:
    /**
     * @brief 构造函数
     * @param config 配置参数
     */
    explicit YoloProducer(const ProducerConfig& config);
    
    ~YoloProducer() override;

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
    const char* GetTypeName() const override { return "YoloV5"; }
    const ProducerConfig& GetConfig() const override { return config_; }

    int SetResolution(Resolution preset) override;
    int SetFrameRate(int fps) override;

private:
    // 禁止拷贝
    YoloProducer(const YoloProducer&) = delete;
    YoloProducer& operator=(const YoloProducer&) = delete;

    /**
     * @brief 初始化 MPI 组件（ISP/VI/VPSS/VENC）
     */
    int InitMpi();

    /**
     * @brief 反初始化 MPI 组件
     */
    int DeinitMpi();

    /**
     * @brief 初始化 AI 引擎
     */
    int InitAiEngine();

    /**
     * @brief 反初始化 AI 引擎
     */
    void DeinitAiEngine();

    /**
     * @brief 初始化 RGB 缓冲池
     */
    bool InitRgbPool();

    /**
     * @brief 帧处理主循环
     */
    void FrameLoop();

    /**
     * @brief 处理单帧（AI 推理 + OSD）
     */
    bool ProcessFrame(void* nv12_data, int width, int height, int stride,
                      void* rgb_output, uint64_t pts);

private:
    ProducerConfig config_;
    
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    
    std::thread frame_thread_;
    
    // 内部实现（PIMPL 模式）
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // 统计
    std::atomic<uint64_t> frame_count_{0};
    std::atomic<uint64_t> inference_count_{0};
};

}  // namespace media
