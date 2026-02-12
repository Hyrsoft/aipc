/**
 * @file rtsp_service.cpp
 * @brief RTSP 推流服务实现
 *
 * @author 好软，好温暖
 * @date 2026-02-04
 */

#define LOG_TAG "rtsp_service"

#include "rtsp_service.h"
#include "common/logger.h"

#include <memory>

// ============================================================================
// RtspService 实现
// ============================================================================

RtspService::RtspService(const RtspConfig& config)
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

RtspService::~RtspService() {
    if (valid_) {
        LOG_INFO("Deinitializing RTSP streaming...");
        rtsp_server_deinit();
        LOG_INFO("RTSP streaming deinitialized");
    }
}

bool RtspService::Start() {
    if (!valid_) {
        LOG_ERROR("RTSP server not initialized, cannot start");
        return false;
    }
    
    if (running_) {
        LOG_WARN("RTSP streaming already running");
        return true;
    }
    
    running_ = true;
    LOG_INFO("RTSP streaming started");
    return true;
}

void RtspService::Stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    LOG_INFO("RTSP streaming stopped");
}

std::string RtspService::GetUrl() const {
    return GetRtspServer().GetUrl();
}

RtspServer::Stats RtspService::GetStats() const {
    return GetRtspServer().GetStats();
}

void RtspService::StreamConsumer(EncodedStreamPtr stream, void* user_data) {
    auto* self = static_cast<RtspService*>(user_data);
    
    // 只有在运行状态下才推送帧
    if (self && self->running_) {
        rtsp_stream_consumer(stream, nullptr);
    }
}

// ============================================================================
// 全局实例管理
// ============================================================================

static std::unique_ptr<RtspService> g_rtsp_service;

RtspService* GetRtspService() {
    return g_rtsp_service.get();
}

void CreateRtspService(const RtspConfig& config) {
    if (g_rtsp_service) {
        LOG_WARN("Global RtspService already exists, destroying old one");
        DestroyRtspService();
    }
    
    g_rtsp_service = std::make_unique<RtspService>(config);
}

void DestroyRtspService() {
    g_rtsp_service.reset();
}

