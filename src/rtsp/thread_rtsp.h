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
     * @brief 获取 RTSP URL
     */
    std::string GetUrl() const;

    /**
     * @brief 获取 RTSP 服务器统计信息
     */
    RtspServer::Stats GetStats() const;

    /**
     * @brief 流消费者回调（用于注册到 StreamDispatcher）
     */
    static void StreamConsumer(EncodedStreamPtr stream, void* user_data);

private:
    RtspConfig config_;
    bool valid_ = false;
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

