/**
 * @file thread_rtsp.h
 * @brief RTSP 推流线程 - 管理 RTSP 服务器和推流逻辑
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <mutex>

#include "rk_rtsp.h"

// ============================================================================
// RTSP 线程类
// ============================================================================

/**
 * @brief RTSP 推流线程
 * 
 * 管理 RTSP 服务器的生命周期和推流逻辑
 * RAII 设计：构造时初始化服务器，析构时清理
 */
class RtspThread {
public:
    /**
     * @brief 构造函数 - 初始化 RTSP 服务器
     * @param config RTSP 配置
     */
    explicit RtspThread(const RtspConfig& config = RtspConfig{});
    
    ~RtspThread();

    // 禁用拷贝
    RtspThread(const RtspThread&) = delete;
    RtspThread& operator=(const RtspThread&) = delete;

    /**
     * @brief 检查是否初始化成功
     */
    bool IsValid() const { return valid_; }

    /**
     * @brief 启动 RTSP 流推送（开始消费视频帧）
     * @return 是否启动成功
     */
    bool Start();

    /**
     * @brief 停止 RTSP 流推送（停止消费视频帧）
     */
    void Stop();

    /**
     * @brief 检查是否正在运行（推送视频帧）
     */
    bool IsRunning() const { return running_; }

    /**
     * @brief 获取 RTSP URL
     */
    std::string GetUrl() const;

    /**
     * @brief 获取 RTSP 服务器统计信息
     */
    RtspServer::Stats GetStats() const;

    /**
     * @brief 流消费者回调（用于注册到 StreamDispatcher）
     * 只有在 running_ 状态下才会推送帧
     */
    static void StreamConsumer(EncodedStreamPtr stream, void* user_data);

private:
    RtspConfig config_;
    bool valid_ = false;
    std::atomic<bool> running_{false};  ///< 是否正在推送视频帧
};

// ============================================================================
// 全局实例管理
// ============================================================================

/**
 * @brief 获取全局 RTSP 线程实例
 */
RtspThread* GetRtspThread();

/**
 * @brief 创建全局 RTSP 线程实例
 * @param config RTSP 配置
 */
void CreateRtspThread(const RtspConfig& config);

/**
 * @brief 销毁全局 RTSP 线程实例
 */
void DestroyRtspThread();

