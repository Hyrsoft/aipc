/**
 * @file http.cpp
 * @brief HTTP API 模块实现
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#define LOG_TAG "http"

#include "http.h"
#include "common/logger.h"
#include "rtsp/thread_rtsp.h"
#include "file/thread_file.h"
#include <httplib.h>
#include <sstream>

// ============================================================================
// 辅助函数
// ============================================================================

/**
 * @brief 生成 JSON 响应
 */
static std::string json_response(bool success, const std::string& message, 
                                  const std::string& data = "") {
    std::ostringstream oss;
    oss << "{\"success\":" << (success ? "true" : "false");
    oss << ",\"message\":\"" << message << "\"";
    if (!data.empty()) {
        oss << ",\"data\":" << data;
    }
    oss << "}";
    return oss.str();
}

// ============================================================================
// HttpApi 实现
// ============================================================================

HttpApi::HttpApi() = default;

HttpApi::~HttpApi() {
    Stop();
}

bool HttpApi::Init(const HttpApiConfig& config, const StreamConfig& stream_config) {
    LOG_INFO("初始化 HTTP API: {}:{}", config.host, config.port);

    config_ = config;
    stream_config_ = stream_config;

    // 创建 HTTP 服务器
    server_ = std::make_unique<HttpServer>();

    HttpServerConfig http_config;
    http_config.host = config.host;
    http_config.port = config.port;
    http_config.static_dir = config.static_dir;
    http_config.static_mount = "/";
    http_config.thread_pool_size = config.thread_pool_size;

    if (!server_->Init(http_config)) {
        LOG_ERROR("HTTP 服务器初始化失败");
        return false;
    }

    // 设置 API 路由
    SetupRoutes();

    LOG_INFO("HTTP API 初始化完成");
    return true;
}

bool HttpApi::Start() {
    if (!server_) {
        LOG_ERROR("HTTP 服务器未初始化");
        return false;
    }

    if (!server_->Start()) {
        LOG_ERROR("HTTP 服务器启动失败");
        return false;
    }

    LOG_INFO("HTTP API 服务已启动: {}:{}", config_.host, config_.port);
    return true;
}

void HttpApi::Stop() {
    if (server_) {
        server_->Stop();
        LOG_INFO("HTTP API 服务已停止");
    }
}

bool HttpApi::IsRunning() const {
    return server_ && server_->IsRunning();
}

void HttpApi::SetupRoutes() {
    if (!server_) {
        return;
    }

    // ========================================================================
    // 系统状态 API
    // ========================================================================
    server_->Get("/api/status", [this](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        std::ostringstream data;
        data << "{";
        
        // RTSP 状态
        data << "\"rtsp\":{";
        data << "\"enabled\":" << (stream_config_.enable_rtsp ? "true" : "false");
        if (mgr && mgr->GetRtspThread()) {
            data << ",\"valid\":" << (mgr->GetRtspThread()->IsValid() ? "true" : "false");
        }
        data << "}";
        
        // WebRTC 状态
        data << ",\"webrtc\":{";
        data << "\"enabled\":" << (stream_config_.enable_webrtc ? "true" : "false");
        if (mgr && mgr->GetWebRTCThread()) {
            data << ",\"running\":" << (mgr->GetWebRTCThread()->IsRunning() ? "true" : "false");
        }
        data << "}";
        
        // 录制状态
        data << ",\"recording\":{";
        data << "\"enabled\":" << (stream_config_.enable_file ? "true" : "false");
        if (mgr && mgr->GetFileThread()) {
            auto* ft = mgr->GetFileThread();
            data << ",\"active\":" << (ft->IsRecording() ? "true" : "false");
            data << ",\"output_dir\":\"" << stream_config_.mp4_config.outputDir << "\"";
        }
        data << "}";
        
        data << "}";
        
        res.set_content(json_response(true, "ok", data.str()), "application/json");
    });

    // ========================================================================
    // RTSP 状态 API
    // ========================================================================
    server_->Get("/api/rtsp/status", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetRtspThread()) {
            res.set_content(json_response(false, "RTSP not available"), "application/json");
            return;
        }
        
        auto* rtsp = mgr->GetRtspThread();
        auto stats = rtsp->GetStats();
        std::ostringstream data;
        data << "{";
        data << "\"valid\":" << (rtsp->IsValid() ? "true" : "false");
        data << ",\"url\":\"" << rtsp->GetUrl() << "\"";
        data << ",\"frames_sent\":" << stats.framesSent;
        data << ",\"bytes_sent\":" << stats.bytesSent;
        data << "}";
        res.set_content(json_response(true, "ok", data.str()), "application/json");
    });

    // ========================================================================
    // WebRTC 控制 API
    // ========================================================================
    server_->Post("/api/webrtc/start", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCThread()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }
        
        auto* webrtc = mgr->GetWebRTCThread();
        if (webrtc->IsRunning()) {
            res.set_content(json_response(true, "WebRTC already running"), "application/json");
            return;
        }
        
        if (webrtc->Start()) {
            res.set_content(json_response(true, "WebRTC started"), "application/json");
        } else {
            res.set_content(json_response(false, "Failed to start WebRTC"), "application/json");
        }
    });
    
    server_->Post("/api/webrtc/stop", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCThread()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }
        
        auto* webrtc = mgr->GetWebRTCThread();
        if (!webrtc->IsRunning()) {
            res.set_content(json_response(true, "WebRTC already stopped"), "application/json");
            return;
        }
        
        webrtc->Stop();
        res.set_content(json_response(true, "WebRTC stopped"), "application/json");
    });

    // ========================================================================
    // 录制控制 API
    // ========================================================================
    server_->Get("/api/record/status", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetFileThread()) {
            res.set_content(json_response(false, "Recording not available"), "application/json");
            return;
        }
        
        auto* ft = mgr->GetFileThread();
        std::ostringstream data;
        data << "{\"recording\":" << (ft->IsRecording() ? "true" : "false") << "}";
        res.set_content(json_response(true, "ok", data.str()), "application/json");
    });
    
    server_->Post("/api/record/start", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetFileThread()) {
            res.set_content(json_response(false, "Recording not available"), "application/json");
            return;
        }
        
        auto* ft = mgr->GetFileThread();
        if (ft->IsRecording()) {
            res.set_content(json_response(true, "Recording already in progress"), "application/json");
            return;
        }
        
        ft->StartRecording();
        res.set_content(json_response(true, "Recording started"), "application/json");
    });
    
    server_->Post("/api/record/stop", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetFileThread()) {
            res.set_content(json_response(false, "Recording not available"), "application/json");
            return;
        }
        
        auto* ft = mgr->GetFileThread();
        if (!ft->IsRecording()) {
            res.set_content(json_response(true, "Recording already stopped"), "application/json");
            return;
        }
        
        ft->StopRecording();
        res.set_content(json_response(true, "Recording stopped"), "application/json");
    });

    LOG_INFO("HTTP API 路由配置完成");
}
