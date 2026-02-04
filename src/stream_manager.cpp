/**
 * @file stream_manager.cpp
 * @brief 视频流分发管理实现
 *
 * @author 好软，好温暖
 * @date 2026-02-04
 */

#define LOG_TAG "stream"

#include "stream_manager.h"
#include "common/logger.h"
#include "rkvideo/rkvideo.h"
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
            // 注册 RTSP 消费者到流分发器
            // 使用 AsyncIO 类型，网络发送通过 asio::post 异步执行
            rkvideo_register_stream_consumer(
                "rtsp",
                &RtspService::StreamConsumer,
                rtsp_service_.get(),
                ConsumerType::AsyncIO,  // 网络发送，投递到 IO 线程
                3
            );
            LOG_INFO("RTSP consumer registered (AsyncIO mode)");
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
        
        // 注册文件消费者到流分发器
        // 使用 Queued 类型，文件写入需要独立线程（磁盘 I/O 延迟不可控）
        rkvideo_register_stream_consumer(
            "file",
            &FileService::StreamConsumer,
            file_service_.get(),
            ConsumerType::Queued,  // 文件写入，使用独立队列和线程
            5  // 队列稍大，避免录制丢帧
        );
        LOG_INFO("File consumer registered (Queued mode)");
    }
    
    // 创建 WebRTC 服务（如果启用）
    if (config_.enable_webrtc) {
        webrtc_service_ = std::make_unique<WebRTCService>(config_.webrtc_config);
        
        // WebRTC 服务需要在 Start() 中才会连接信令服务器
        // 使用 AsyncIO 类型，网络发送通过 asio::post 异步执行
        rkvideo_register_stream_consumer(
            "webrtc",
            &WebRTCService::StreamConsumer,
            webrtc_service_.get(),
            ConsumerType::AsyncIO,  // 网络发送，投递到 IO 线程
            3
        );
        LOG_INFO("WebRTC consumer registered (AsyncIO mode, will connect on Start)");
    }
    
    // 创建 WebSocket 预览服务器（如果启用）
    if (config_.enable_ws_preview) {
        ws_preview_server_ = std::make_unique<WsPreviewServer>(config_.ws_preview_config);
        
        // 注册 WebSocket 预览消费者到流分发器
        // 使用 AsyncIO 类型，WebSocket 发送通过 asio::post 异步执行
        rkvideo_register_stream_consumer(
            "ws_preview",
            &WsPreviewServer::StreamConsumer,
            ws_preview_server_.get(),
            ConsumerType::AsyncIO,  // 网络发送，投递到 IO 线程
            3
        );
        LOG_INFO("WebSocket preview consumer registered (AsyncIO mode)");
    }
    
    LOG_INFO("StreamManager created with optimized consumer types");
}

StreamManager::~StreamManager() {
    Stop();
    LOG_INFO("StreamManager destroyed");
}

void StreamManager::Start() {
    if (running_) {
        LOG_WARN("StreamManager already running");
        return;
    }
    
    LOG_INFO("Starting stream outputs...");
    
    // 启动文件服务（如果存在）
    if (file_service_) {
        file_service_->Start();
    }
    
    // 启动 WebSocket 预览服务器（如果存在）
    if (ws_preview_server_) {
        ws_preview_server_->Start();
    }
    
    // 注意：RTSP 和 WebRTC 默认不自动启动，需要通过 API 手动启动
    // 这样可以避免在没有客户端连接时浪费资源
    
    // 启动视频流分发器
    rkvideo_start_streaming();
    
    running_ = true;
    LOG_INFO("Stream outputs started");
}

void StreamManager::Stop() {
    if (!running_) {
        return;
    }
    
    LOG_INFO("Stopping stream outputs...");
    
    // 停止视频流分发器
    rkvideo_stop_streaming();
    
    // 停止 RTSP 服务
    if (rtsp_service_) {
        rtsp_service_->Stop();
    }
    
    // 停止文件服务
    if (file_service_) {
        file_service_->Stop();
    }
    
    // 停止 WebRTC 服务
    if (webrtc_service_) {
        webrtc_service_->Stop();
    }
    
    // 停止 WebSocket 预览服务器
    if (ws_preview_server_) {
        ws_preview_server_->Stop();
    }
    
    running_ = false;
    LOG_INFO("Stream outputs stopped");
}

// ============================================================================
// 全局实例管理
// ============================================================================

static std::unique_ptr<StreamManager> g_stream_manager;

StreamManager* GetStreamManager() {
    return g_stream_manager.get();
}

void CreateStreamManager(const StreamConfig& config) {
    if (g_stream_manager) {
        LOG_WARN("Global StreamManager already exists, destroying old one");
        DestroyStreamManager();
    }
    
    g_stream_manager = std::make_unique<StreamManager>(config);
}

void DestroyStreamManager() {
    g_stream_manager.reset();
}

