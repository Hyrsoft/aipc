/**
 * @file serial_pipeline.h
 * @brief 串行管道处理器 - Mode B: Inference AI
 *
 * 实现软件控制的串行帧处理流水线：
 * VI -> VPSS (绑定) -> [手动获取] -> AI推理 -> OSD -> [手动发送] -> VENC
 *
 * 与并行模式的区别：
 * - 并行模式: VPSS 和 VENC 硬件绑定，零拷贝
 * - 串行模式: 解绑 VPSS-VENC，由软件控制帧流动时序
 *
 * @author 好软，好温暖
 * @date 2026-02-11
 */

#pragma once

#include "pipeline_mode.h"
#include "stream_dispatcher.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <memory>

namespace rkvideo {

/**
 * @brief 串行管道配置
 */
struct SerialPipelineConfig {
    int width = 1920;               ///< 视频宽度
    int height = 1080;              ///< 视频高度
    int frame_rate = 30;            ///< 帧率
    int frame_timeout_ms = 100;     ///< 获取帧超时（毫秒）
    int ai_width = 640;             ///< AI 处理分辨率宽度
    int ai_height = 640;            ///< AI 处理分辨率高度
};

/**
 * @class SerialPipeline
 * @brief 串行管道处理器
 *
 * 负责 Mode B (Inference AI) 的帧处理循环：
 * 1. 从 VPSS 手动获取帧
 * 2. 调用用户回调进行 AI 推理 + OSD 叠加
 * 3. 将处理后的帧手动送入 VENC
 * 4. 将编码结果分发给消费者
 */
class SerialPipeline {
public:
    SerialPipeline();
    ~SerialPipeline();

    // 禁止拷贝
    SerialPipeline(const SerialPipeline&) = delete;
    SerialPipeline& operator=(const SerialPipeline&) = delete;

    /**
     * @brief 初始化串行管道
     * 
     * 初始化 VI、VPSS、VENC，但不绑定 VPSS-VENC
     * 只绑定 VI-VPSS，VENC 采用 SendFrame 模式
     * 
     * @param config 配置参数
     * @return 0 成功，-1 失败
     */
    int Init(const SerialPipelineConfig& config);

    /**
     * @brief 反初始化串行管道
     * @return 0 成功
     */
    int Deinit();

    /**
     * @brief 启动帧处理循环
     * 
     * @param process_callback 帧处理回调（AI 推理 + OSD）
     * @return true 成功启动
     */
    bool Start(FrameProcessCallback process_callback);

    /**
     * @brief 停止帧处理循环
     */
    void Stop();

    /**
     * @brief 检查是否正在运行
     */
    bool IsRunning() const { return running_.load(); }

    /**
     * @brief 注册编码流消费者
     */
    void RegisterStreamConsumer(const char* name, 
                                 std::function<void(EncodedStreamPtr)> callback,
                                 ConsumerType type = ConsumerType::AsyncIO,
                                 int queue_size = 3);

    /**
     * @brief 获取配置
     */
    const SerialPipelineConfig& GetConfig() const { return config_; }

private:
    // 前向声明内部实现类
    struct Impl;

    /**
     * @brief 帧处理主循环
     */
    void FrameLoop();

    /**
     * @brief 初始化 RGB 缓冲池
     */
    bool InitRgbPool();

    /**
     * @brief 初始化 MPI 组件（VI/VPSS/VENC）
     */
    int InitMpi();

    /**
     * @brief 反初始化 MPI 组件
     */
    int DeinitMpi();

    SerialPipelineConfig config_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::thread frame_thread_;
    
    FrameProcessCallback process_callback_;
    std::mutex callback_mutex_;

    // 流分发器
    std::unique_ptr<StreamDispatcher> dispatcher_;

    // 内部实现（PIMPL 模式）
    std::unique_ptr<Impl> impl_;

    // 统计信息
    std::atomic<uint64_t> frame_count_{0};
    std::atomic<uint64_t> encode_count_{0};
};

}  // namespace rkvideo
