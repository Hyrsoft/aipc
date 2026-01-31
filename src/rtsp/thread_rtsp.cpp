/**
 * @file thread_rtsp.cpp
 * @brief RTSP 推流线程实现
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#define LOG_TAG "thread_rtsp"

#include "thread_rtsp.h"
#include "common/logger.h"

#include <memory>

// ============================================================================
// RtspThread 实现
// ============================================================================

RtspThread::RtspThread(const RtspConfig& config)
    : config_(config) {
    LOG_INFO("Initializing RTSP streaming...");
    
    // 初始化 RTSP 服务器
    if (!rtsp_server_init(config_)) {
        LOG_ERROR("Failed to initialize RTSP server");
        return;
    }
    
    valid_ = true;
    LOG_INFO("RTSP streaming initialized, URL: {}", GetRtspServer().GetUrl());
}

RtspThread::~RtspThread() {
    if (valid_) {
        LOG_INFO("Deinitializing RTSP streaming...");
        rtsp_server_deinit();
        LOG_INFO("RTSP streaming deinitialized");
    }
}

std::string RtspThread::GetUrl() const {
    return GetRtspServer().GetUrl();
}

RtspServer::Stats RtspThread::GetStats() const {
    return GetRtspServer().GetStats();
}

void RtspThread::StreamConsumer(EncodedStreamPtr stream, void* /* user_data */) {
    rtsp_stream_consumer(stream, nullptr);
}

// ============================================================================
// 全局实例管理
// ============================================================================

static std::unique_ptr<RtspThread> g_rtsp_thread;

RtspThread* GetRtspThread() {
    return g_rtsp_thread.get();
}

void CreateRtspThread(const RtspConfig& config) {
    if (g_rtsp_thread) {
        LOG_WARN("Global RtspThread already exists, destroying old one");
        DestroyRtspThread();
    }
    
    g_rtsp_thread = std::make_unique<RtspThread>(config);
}

void DestroyRtspThread() {
    g_rtsp_thread.reset();
}

