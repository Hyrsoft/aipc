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
 * 分辨率切换：
 * - 支持 simple_ipc::Resolution 预设（R_1080P / R_720P / R_480P）
 * - 切换分辨率需要重新初始化 MPI 管线（冷切换），通过 SetResolution() 实现
 * - SetResolution() / SetFrameRate() 是具体方法（非虚），不属于 IMediaProducer 共有接口
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#pragma once

#include "../i_media_producer.h"
#include "simple_ipc_config.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace media {

    /**
     * @class SimpleIPCProducer
     * @brief 纯监控模式生产者
     *
     * 实现最简单、最高效的视频采集与编码链路。
     * 硬件绑定，零拷贝，CPU 不参与数据流。
     *
     * 分辨率等 SimpleIPC 专有参数通过 SimpleIPCConfig 在构造时传入，
     * 也可在 Stop() 后通过 SetResolution() / SetFrameRate() 修改，
     * 修改后需重新 Init()（或由 MediaManager::SetResolution() 统一完成）。
     */
    class SimpleIPCProducer : public IMediaProducer {
    public:
        /**
         * @brief 构造函数
         * @param config SimpleIPC 配置（包含分辨率预设、帧率、码率）
         */
        explicit SimpleIPCProducer(const SimpleIPCConfig &config);

        ~SimpleIPCProducer() override;

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
        const char *GetTypeName() const override { return "SimpleIPC"; }

        /**
         * @brief 获取当前通用配置（framerate、bitrate_kbps）
         *
         * 若需获取分辨率预设，请使用 GetSimpleIPCConfig()。
         */
        const ProducerConfig &GetConfig() const override { return shared_config_; }

        // ========== SimpleIPC 专有接口（非虚）==========

        /**
         * @brief 获取 SimpleIPC 完整配置（含分辨率预设）
         */
        const SimpleIPCConfig &GetSimpleIPCConfig() const { return config_; }

        /**
         * @brief 设置分辨率预设
         *
         * 仅在 Stop() 后调用有效（运行中不允许修改）。
         * 修改后需重新调用 Init() 才能生效（MediaManager::SetResolution() 会自动处理）。
         *
         * @param preset 分辨率预设
         * @return 0 成功，-1 正在运行中（不允许修改）
         */
        int SetResolution(simple_ipc::Resolution preset);

        /**
         * @brief 设置帧率
         *
         * 仅在 Stop() 后调用有效。
         *
         * @param fps 帧率（1-30）
         * @return 0 成功，-1 正在运行中
         */
        int SetFrameRate(int fps);

    private:
        // 禁止拷贝
        SimpleIPCProducer(const SimpleIPCProducer &) = delete;
        SimpleIPCProducer &operator=(const SimpleIPCProducer &) = delete;

        /**
         * @brief 初始化 MPI 组件（ISP/VI/VPSS/VENC）
         */
        int InitMpi();

        /**
         * @brief 反初始化 MPI 组件
         */
        int DeinitMpi();

        /**
         * @brief 建立绑定关系（VI->VPSS->VENC）
         */
        int SetupBindings();

        /**
         * @brief 解除绑定关系
         */
        void TeardownBindings();

    private:
        SimpleIPCConfig config_; ///< SimpleIPC 完整配置（含分辨率预设）
        ProducerConfig shared_config_; ///< 共有配置视图（供 IMediaProducer::GetConfig() 返回）

        std::atomic<bool> initialized_{false};
        std::atomic<bool> running_{false};

        // 内部实现（PIMPL 模式，隐藏 MPI 依赖）
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace media
