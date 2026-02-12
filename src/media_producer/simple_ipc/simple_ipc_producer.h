/**
 * @file simple_ipc_producer.h
 * @brief 纯监控模式生产者 - 零拷贝硬件绑定
 *
 * 数据流架构：
 *   VI (单通道) --> VPSS Group0 --> VENC --> 编码流
 *                      ^
 *                      |
 *                   (硬件绑定，零拷贝)
 *
 * 特点：
 * - 完全硬件绑定，VI -> VPSS -> VENC 零拷贝
 * - CPU 几乎不参与数据流
 * - 总线压力最大（VI/VPSS/VENC 并行访问 DDR）
 * - 适合无 AI 推理的纯流媒体场景
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#pragma once

#include "../i_media_producer.h"

#include <memory>
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>

namespace media {

/**
 * @class SimpleIPCProducer
 * @brief 纯监控模式生产者
 *
 * 实现最简单、最高效的视频采集与编码链路。
 * 硬件绑定，零拷贝，CPU 不参与数据流。
 */
class SimpleIPCProducer : public IMediaProducer {
public:
    /**
     * @brief 构造函数
     * @param config 配置参数
     */
    explicit SimpleIPCProducer(const ProducerConfig& config);
    
    ~SimpleIPCProducer() override;

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
    const char* GetTypeName() const override { return "SimpleIPC"; }
    const ProducerConfig& GetConfig() const override { return config_; }

    int SetResolution(Resolution preset) override;
    int SetFrameRate(int fps) override;

private:
    // 禁止拷贝
    SimpleIPCProducer(const SimpleIPCProducer&) = delete;
    SimpleIPCProducer& operator=(const SimpleIPCProducer&) = delete;

    /**
     * @brief 初始化 MPI 组件（ISP/VI/VPSS/VENC）
     */
    int InitMpi();

    /**
     * @brief 反初始化 MPI 组件
     */
    int DeinitMpi();

    /**
     * @brief 建立绑定关系
     */
    int SetupBindings();

    /**
     * @brief 解除绑定关系
     */
    void TeardownBindings();

private:
    ProducerConfig config_;
    
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    
    // 内部实现（PIMPL 模式，隐藏 MPI 依赖）
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace media
