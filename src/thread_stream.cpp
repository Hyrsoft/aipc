/**
 * @file thread_stream.cpp
 * @brief 流处理线程 - 管理 RTSP/WebRTC/文件保存等流输出
 *
 * 实现编码流的分发和处理，支持：
 * - RTSP 推流
 * - WebRTC 推流（待实现）
 * - 文件保存（待实现）
 *
 * 架构说明：
 * - 使用 StreamDispatcher 的消费者回调模式
 * - 每个输出类型注册为独立消费者
 * - 实现零拷贝的流分发
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

// 定义模块名，必须在 #include "logger.h" 之前
#define LOG_TAG "stream"

#include "thread_stream.h"
#include "common/logger.h"
#include "rkvideo/rkvideo.h"
#include "rtsp/rk_rtsp.h"

// ============================================================================
// RTSP 推流
// ============================================================================

bool stream_rtsp_init(const RtspConfig& config) {
    LOG_INFO("Initializing RTSP streaming...");
    
    // 初始化 RTSP 服务器
    if (!rtsp_server_init(config)) {
        LOG_ERROR("Failed to initialize RTSP server");
        return false;
    }
    
    // 注册到视频流分发器
    // 使用 C 风格回调函数包装器
    rkvideo_register_stream_consumer(
        "rtsp",
        [](EncodedStreamPtr stream, void* userData) {
            rtsp_stream_consumer(stream, userData);
        },
        nullptr,
        3  // 队列大小
    );
    
    LOG_INFO("RTSP streaming initialized, URL: {}", GetRtspServer().GetUrl());
    return true;
}

void stream_rtsp_deinit() {
    LOG_INFO("Deinitializing RTSP streaming...");
    rtsp_server_deinit();
    LOG_INFO("RTSP streaming deinitialized");
}

// ============================================================================
// WebRTC 推流（待实现）
// ============================================================================

bool stream_webrtc_init() {
    LOG_WARN("WebRTC streaming not implemented yet");
    // TODO: 实现 WebRTC 推流
    // 1. 初始化 WebRTC 服务
    // 2. 注册消费者回调
    return false;
}

void stream_webrtc_deinit() {
    // TODO: 实现 WebRTC 清理
}

// ============================================================================
// 文件保存（待实现）
// ============================================================================

bool stream_file_start(const char* /* filename */) {
    LOG_WARN("File recording not implemented yet");
    // TODO: 实现文件保存
    // 1. 打开文件
    // 2. 注册消费者回调
    return false;
}

void stream_file_stop() {
    // TODO: 实现停止录制
}

// ============================================================================
// 统一流管理
// ============================================================================

static StreamOutputConfig g_streamConfig;

bool stream_manager_init(const StreamOutputConfig& config) {
    g_streamConfig = config;
    bool success = true;
    
    LOG_INFO("Initializing stream manager...");
    
    // 初始化 RTSP（如果启用）
    if (config.enableRtsp) {
        if (!stream_rtsp_init(config.rtspConfig)) {
            LOG_ERROR("Failed to initialize RTSP output");
            success = false;
        }
    }
    
    // 初始化 WebRTC（如果启用）
    if (config.enableWebrtc) {
        if (!stream_webrtc_init()) {
            LOG_WARN("WebRTC output not available");
            // 不算失败，继续运行
        }
    }
    
    // 初始化文件保存（如果启用）
    if (config.enableFile && !config.filePath.empty()) {
        if (!stream_file_start(config.filePath.c_str())) {
            LOG_WARN("File recording not available");
            // 不算失败，继续运行
        }
    }
    
    if (success) {
        LOG_INFO("Stream manager initialized successfully");
    }
    
    return success;
}

void stream_manager_deinit() {
    LOG_INFO("Deinitializing stream manager...");
    
    stream_file_stop();
    stream_webrtc_deinit();
    stream_rtsp_deinit();
    
    LOG_INFO("Stream manager deinitialized");
}

void stream_manager_start() {
    LOG_INFO("Starting stream outputs...");
    rkvideo_start_streaming();
    LOG_INFO("Stream outputs started");
}

void stream_manager_stop() {
    LOG_INFO("Stopping stream outputs...");
    rkvideo_stop_streaming();
    LOG_INFO("Stream outputs stopped");
}
