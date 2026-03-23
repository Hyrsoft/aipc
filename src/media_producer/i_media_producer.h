/**
 * @file i_media_producer.h
 * @brief 媒体生产者接口 - 定义统一的视频采集与编码接口
 *
 * 设计模式：策略模式 (Strategy Pattern)
 *
 * 核心设计思想：
 * - 外部接口统一，内部硬件资源分配逻辑完全独立且差异巨大
 * - 每个子类是一个独立的"黑盒"，内部包含各自的配置与资源管理
 * - 运行时通过多态 (vtable) 动态分发到具体实现
 *
 * 子类实现：
 * - SimpleIPCProducer: 纯监控模式（VI -> VPSS -> VENC 硬件绑定，零拷贝）
 *   分辨率由 SimpleIPCConfig 决定，支持预设切换（需重新初始化）。
 * - VisionGProducer: Python 驱动的 AI 视觉模式
 *   Python 脚本全权负责摄像头（visiong.Camera）、帧循环、模型推理与分辨率；
 *   C++ 负责 Python 解释器生命周期、VENC 编码和流媒体分发。
 *
 * 注意：分辨率相关配置（Resolution 枚举、ResolutionConfig 结构体、
 * SimpleIPCConfig）定义在 simple_ipc/simple_ipc_config.h，
 * 仅属于 SimpleIPC 模式，不在本共有接口中暴露。
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
        Direct, ///< 直接在 Fetch 线程中执行（仅用于极快的操作）
        AsyncIO, ///< 通过 asio::post 投递到 IO 线程执行（网络发送）
        Queued ///< 通过队列投递到独立线程（文件写入等阻塞操作）
    };

    /**
     * @brief 编码流回调类型
     */
    using StreamCallback = std::function<void(EncodedStreamPtr)>;

    // ============================================================================
    // 生产者通用配置
    // ============================================================================

    /**
     * @brief 媒体生产者通用配置
     *
     * 仅包含两种模式共同需要的参数。
     * - SimpleIPC 专有的分辨率配置见 simple_ipc/simple_ipc_config.h 中的
     *   SimpleIPCConfig。
     * - VisionG 的摄像头分辨率由 Python 脚本通过 visiong.Camera(w, h, ...)
     *   自行决定，C++ 层无需感知。
     */
    struct ProducerConfig {
        int framerate = 30;
        int bitrate_kbps = 10 * 1024; // 10 Mbps
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
     * 2. Init() - 初始化硬件/软件资源
     * 3. RegisterStreamConsumer() - 注册编码流消费者
     *
 4. Start() - 启动视频流
     * 5. Stop() - 停止视频流
     * 6. Deinit() - 释放资源
     * 7. 析构函数 - 销毁实例
     *
     * 注意：分辨率等模式专有配置不在此接口中暴露。
     * SimpleIPC 通过 SimpleIPCConfig 在构造时传入分辨率，
     * 并通过 SimpleIPCProducer 的具体方法（非虚）进行切换。
     */
    class IMediaProducer {
    public:
        virtual ~IMediaProducer() = default;

        // ========== 生命周期管理 ==========

        /**
         * @brief 初始化媒体生产者
         *
         * 分配资源，建立视频采集与编码链路。
         * - SimpleIPC：分配 ISP/VI/VPSS/VENC 等硬件资源。
         * - VisionG：初始化 Python 运行时，加载初始脚本。
         *
         * @return 0 成功，-1 失败
         */
        virtual int Init() = 0;

        /**
         * @brief 反初始化媒体生产者
         *
         * 释放所有资源，停止视频采集与编码。
         *
         * @return 0 成功
         */
        virtual int Deinit() = 0;

        /**
         * @brief 启动视频流
         *
         * 开始视频采集、编码和分发。
         * - SimpleIPC：启动 VENC fetch 线程。
         * - VisionG：在后台线程中启动 Python 脚本的 run() 函数。
         *
         * @return true 成功，false 失败
         */
        virtual bool Start() = 0;

        /**
         * @brief 停止视频流
         *
         * 停止采集和分发，但不释放资源。
         * - VisionG：向 Python 的 aipc.is_running() 发送停止信号，
         *   等待 Python run() 函数返回。
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
        virtual void RegisterStreamConsumer(const std::string &name, StreamCallback callback,
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
        virtual const char *GetTypeName() const = 0;

        /**
         * @brief 获取当前通用配置
         *
         * 返回 framerate、bitrate_kbps 等共有参数。
         * 模式专有配置（分辨率等）通过具体子类接口获取。
         */
        virtual const ProducerConfig &GetConfig() const = 0;

    protected:
        IMediaProducer() = default;

        // 禁止拷贝
        IMediaProducer(const IMediaProducer &) = delete;
        IMediaProducer &operator=(const IMediaProducer &) = delete;
    };

    // ============================================================================
    // 工厂函数
    // ============================================================================

    // 前向声明
    struct SimpleIPCConfig;

    /**
     * @brief 创建纯 IPC 模式生产者
     *
     * 特点：
     * - VI -> VPSS -> VENC 硬件绑定，零拷贝
     * - CPU 不参与数据流
     * - 最高性能，最低延迟
     *
     * @param config SimpleIPC 配置（包含分辨率预设、帧率、码率）
     * @return 生产者实例
     */
    std::unique_ptr<IMediaProducer> CreateSimpleIPCProducer(const SimpleIPCConfig &config);

    /**
     * @brief 创建 VisionG Python 驱动模式生产者
     *
     * Python 脚本全权负责：摄像头初始化（visiong.Camera）、帧循环、
     * 模型推理与绘制，并通过 aipc.submit_frame() 将处理后的帧提交给
     * C++ 进行 VENC 编码和流媒体分发。
     *
     * C++ 负责：Python 解释器生命周期管理、Python 工程的加载与热更新、
     * VENC 编码、流媒体分发（RTSP/WebRTC 等）。
     *
     * @param config 通用配置（framerate、bitrate_kbps）
     * @return 生产者实例
     */
    std::unique_ptr<IMediaProducer> CreateVisionGProducer(const ProducerConfig &config);

    // ============================================================================
    // 生产者模式枚举
    // ============================================================================

    /**
     * @brief 生产者模式枚举
     */
    enum class ProducerMode {
        SimpleIPC, ///< 纯监控模式（高清、低延迟、零 CPU 拷贝）
        VisionG ///< Python 驱动的 AI 视觉模式
    };

    /**
     * @brief 模式枚举转字符串
     */
    inline const char *ProducerModeToString(ProducerMode mode) {
        switch (mode) {
            case ProducerMode::SimpleIPC:
                return "SimpleIPC";
            case ProducerMode::VisionG:
                return "VisionG";
            default:
                return "Unknown";
        }
    }

} // namespace media
