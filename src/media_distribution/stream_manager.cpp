/**
 * @file stream_manager.cpp
 * @brief 视频流分发管理实现
 *
 * 使用新的 MediaManager (Producer-based) 架构
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#define LOG_TAG "stream"

#include "stream_manager.h"
#include "common/logger.h"
#include "rtsp/rtsp_service.h"
#include "file/file_service.h"
#include "webrtc/webrtc_service.h"
#include "wspreview/ws_preview.h"

#include <memory>

// ============================================================================
// StreamManager 实现
// ============================================================================

StreamManager::StreamManager(const StreamConfig& config)
    : config_(config) {
    
    LOG_INFO("Creating StreamManager (optimized for single-core CPU)...");
    
    // 创建 RTSP 服务（如果启用）
    if (config_.enable_rtsp) {
        rtsp_service_ = std::make_unique<RtspService>(config_.rtsp_config);
        
        if (rtsp_service_->IsValid()) {
            LOG_INFO("RTSP service created");
        } else {
            LOG_ERROR("Failed to create RTSP service");
            rtsp_service_.reset();
        }
    }
    
    // 创建文件保存服务（如果启用）
    if (config_.enable_file) {
        FileServiceConfig file_config;
        file_config.mp4Config = config_.mp4_config;
        
        file_service_ = std::make_unique<FileService>(file_config);
        LOG_INFO("File service created");
    }
    
    // 创建 WebRTC 服务（如果启用）
    if (config_.enable_webrtc) {
        webrtc_service_ = std::make_unique<WebRTCService>(config_.webrtc_config);
        LOG_INFO("WebRTC service created");
    }
    
    // 创建 WebSocket 预览服务器（如果启用）
    if (config_.enable_ws_preview) {
        ws_preview_server_ = std::make_unique<WsPreviewServer>(config_.ws_preview_config);
        LOG_INFO("WebSocket preview server created");
    }
    
    LOG_INFO("StreamManager created");
}

StreamManager::~StreamManager() {
    Stop();
    LOG_INFO("StreamManager destroyed");
}

void StreamManager::Start() {
    LOG_INFO("Starting StreamManager...");
    
    // 启动 RTSP
    if (rtsp_service_ && rtsp_service_->IsValid()) {
        if (rtsp_service_->Start()) {
            LOG_INFO("RTSP service started: {}", rtsp_service_->GetUrl());
        } else {
            LOG_ERROR("Failed to start RTSP service");
        }
    }
    
    // 启动 WebRTC
    if (webrtc_service_) {
        if (webrtc_service_->Start()) {
            LOG_INFO("WebRTC service started");
        } else {
            LOG_ERROR("Failed to start WebRTC service");
        }
    }
    
    // WebSocket 预览自动启动
    if (ws_preview_server_) {
        LOG_INFO("WebSocket preview server ready on port {}", 
                 config_.ws_preview_config.port);
    }
    
    LOG_INFO("StreamManager started");
}

void StreamManager::Stop() {
    LOG_INFO("Stopping StreamManager...");
    
    // 停止录制
    if (file_service_ && file_service_->IsRecording()) {
        file_service_->StopRecording();
    }
    
    // 停止 RTSP
    if (rtsp_service_) {
        rtsp_service_->Stop();
    }
    
    // 停止 WebRTC
    if (webrtc_service_) {
        webrtc_service_->Stop();
    }
    
    LOG_INFO("StreamManager stopped");
}

// ============================================================================
// 全局 StreamManager 实例管理
// ============================================================================

static std::unique_ptr<StreamManager> g_stream_manager;

void CreateStreamManager(const StreamConfig& config) {
    g_stream_manager = std::make_unique<StreamManager>(config);
}

StreamManager* GetStreamManager() {
    return g_stream_manager.get();
}

void DestroyStreamManager() {
    g_stream_manager.reset();
}

