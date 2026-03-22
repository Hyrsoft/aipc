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
 * 不允许在 Python 脚本中自建 Camera 或帧循环主控。
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

    // 预热内嵌 Python 解释器与 visiong 模块，建议在主线程启动早期调用一次。
    void WarmupVisionGPythonRuntime();

    /**
     * @class VisionGProducer
     * @brief C++ 驱动帧循环、Python 负责处理的 VisionG 生产者
     *
     * 内部流水线：
     *   Camera::snapshot() --> PythonRuntime::ProcessFrame(frame) --> VencManager::encodeToVideo() --> dispatch
     *
     * 并发安全（Phase A 修复）：
     *   - impl_->runtime 为 shared_ptr，FrameLoop 每次迭代持有局部引用副本。
     *   - UpdateCode 仅在极短区间持有 state_mutex_，LoadCode 由 PythonRuntime
     *     内部 mutex + GIL 自行序列化，不与 state_mutex_ 形成嵌套持锁。
     *   - LoadCode 先调旧 cleanup()（尽力）、清空旧 state，再 exec + 验签名 +
     *     调新 init()，全部成功后才提交，init() 失败时自动回滚并返回 [init error]。
     */
    class VisionGProducer : public IMediaProducer {
    public:
        explicit VisionGProducer(const ProducerConfig &config);

        ~VisionGProducer() override;

        // ========== IMediaProducer 接口实现 ==========

        int Init() override;
        int Deinit() override;
        bool Start() override;
        void Stop() override;

        void RegisterStreamConsumer(const std::string &name, StreamCallback callback,
                                    StreamConsumerType type = StreamConsumerType::AsyncIO, int queue_size = 3) override;

        void ClearStreamConsumers() override;

        bool IsInitialized() const override { return initialized_.load(); }
        bool IsRunning() const override { return running_.load(); }
        const char *GetTypeName() const override { return type_name_.c_str(); }
        const ProducerConfig &GetConfig() const override { return config_; }

        int SetResolution(Resolution preset) override;
        int SetFrameRate(int fps) override;

        std::string GetCurrentCode() const;
        std::string GetLastError() const;
        std::string UpdateCode(const std::string &code);

    private:
        // 禁止拷贝
        VisionGProducer(const VisionGProducer &) = delete;
        VisionGProducer &operator=(const VisionGProducer &) = delete;

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
