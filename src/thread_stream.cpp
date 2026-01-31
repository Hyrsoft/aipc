/**
 * @file thread_stream.cpp
 * @brief 视频流分发管理实现
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#define LOG_TAG "stream"

#include "thread_stream.h"
#include "common/logger.h"
#include "rkvideo/rkvideo.h"
#include "rtsp/thread_rtsp.h"
#include "file/thread_file.h"
#include "webrtc/thread_webrtc.h"

#include <memory>

// ============================================================================
// StreamManager 实现
// ============================================================================

StreamManager::StreamManager(const StreamConfig& config)
    : config_(config) {
    
    LOG_INFO("Creating StreamManager...");
    
    // 创建 RTSP 线程（如果启用）
    if (config_.enable_rtsp) {
        rtsp_thread_ = std::make_unique<RtspThread>(config_.rtsp_config);
        
        if (rtsp_thread_->IsValid()) {
            // 注册 RTSP 消费者到流分发器
            rkvideo_register_stream_consumer(
                "rtsp",
                &RtspThread::StreamConsumer,
                rtsp_thread_.get(),
                3  // 队列大小
            );
            LOG_INFO("RTSP consumer registered");
        } else {
            LOG_ERROR("Failed to create RTSP thread");
            rtsp_thread_.reset();
        }
    }
    
    // 创建文件保存线程（如果启用）
    if (config_.enable_file) {
        FileThreadConfig file_config;
        file_config.mp4Config = config_.mp4_config;
        
        file_thread_ = std::make_unique<FileThread>(file_config);
        
        // 注册文件消费者到流分发器
        rkvideo_register_stream_consumer(
            "file",
            &FileThread::StreamConsumer,
            file_thread_.get(),
            5  // 队列大小稍大，避免录制丢帧
        );
        LOG_INFO("File consumer registered");
    }
    
    // 创建 WebRTC 线程（如果启用）
    if (config_.enable_webrtc) {
        webrtc_thread_ = std::make_unique<WebRTCThread>(config_.webrtc_config);
        
        if (webrtc_thread_->IsValid()) {
            // 注册 WebRTC 消费者到流分发器
            rkvideo_register_stream_consumer(
                "webrtc",
                &WebRTCThread::StreamConsumer,
                webrtc_thread_.get(),
                3  // 队列大小
            );
            LOG_INFO("WebRTC consumer registered");
        } else {
            LOG_ERROR("Failed to create WebRTC thread");
            webrtc_thread_.reset();
        }
    }
    
    LOG_INFO("StreamManager created");
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
    
    // 启动文件线程（如果存在）
    if (file_thread_) {
        file_thread_->Start();
    }
    
    // 启动 WebRTC 线程（如果存在）
    if (webrtc_thread_) {
        webrtc_thread_->Start();
    }
    
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
    
    // 停止文件线程
    if (file_thread_) {
        file_thread_->Stop();
    }
    
    // 停止 WebRTC 线程
    if (webrtc_thread_) {
        webrtc_thread_->Stop();
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

