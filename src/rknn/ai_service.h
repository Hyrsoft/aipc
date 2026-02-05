/**
 * @file ai_service.h
 * @brief AI 推理服务 - 管理推理线程和结果分发
 *
 * 从 VPSS Chn1 获取原始帧，执行 AI 推理，并分发检测结果。
 * 
 * 设计考虑：
 * - RV1106 单核 CPU，需要控制推理频率避免影响编码
 * - 使用低优先级线程，让编码任务优先执行
 * - 支持跳帧推理（每 N 帧推理一次）
 *
 * @author 好软，好温暖
 * @date 2026-02-05
 */

#pragma once

#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include "ai_types.h"

namespace rknn {

/**
 * @brief AI 检测结果回调类型
 */
using DetectionCallback = std::function<void(const DetectionResultList&, uint64_t timestamp)>;

/**
 * @brief AI 服务配置
 */
struct AIServiceConfig {
    int skip_frames = 2;        ///< 跳帧数，每 N 帧推理一次（0=每帧都推理）
    int timeout_ms = 100;       ///< 获取帧超时（毫秒）
    bool enable_log = false;    ///< 是否打印推理日志
};

/**
 * @class AIService
 * @brief AI 推理服务
 *
 * 管理 AI 推理线程的生命周期，从 VPSS 获取帧并执行推理。
 */
class AIService {
public:
    /**
     * @brief 获取单例实例
     */
    static AIService& Instance();

    // 禁止拷贝
    AIService(const AIService&) = delete;
    AIService& operator=(const AIService&) = delete;

    /**
     * @brief 启动 AI 推理服务
     * 
     * @param config 服务配置
     * @return true 成功，false 失败
     */
    bool Start(const AIServiceConfig& config = AIServiceConfig{});

    /**
     * @brief 停止 AI 推理服务
     */
    void Stop();

    /**
     * @brief 检查服务是否正在运行
     */
    bool IsRunning() const { return running_.load(); }

    /**
     * @brief 注册检测结果回调
     * 
     * @param callback 回调函数
     */
    void RegisterCallback(DetectionCallback callback);

    /**
     * @brief 清除所有回调
     */
    void ClearCallbacks();

    /**
     * @brief 获取推理统计信息
     */
    struct Stats {
        uint64_t frames_processed = 0;  ///< 已处理帧数
        uint64_t frames_skipped = 0;    ///< 已跳过帧数
        uint64_t total_detections = 0;  ///< 总检测数
        double avg_inference_ms = 0.0;  ///< 平均推理时间（毫秒）
    };
    Stats GetStats() const;

private:
    AIService() = default;
    ~AIService();

    /**
     * @brief 推理线程循环
     */
    void InferenceLoop();

private:
    std::thread thread_;
    std::atomic<bool> running_{false};
    AIServiceConfig config_;

    mutable std::mutex callback_mutex_;
    std::vector<DetectionCallback> callbacks_;

    // 统计信息
    mutable std::mutex stats_mutex_;
    Stats stats_;
};

}  // namespace rknn
