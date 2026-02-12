/**
 * @file http.cpp
 * @brief HTTP API 模块实现
 *
 * 使用新的 MediaManager (Producer-based) 架构
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#define LOG_TAG "http"

#include "http.h"
#include "common/logger.h"
#include "media_distribution/rtsp/rtsp_service.h"
#include "media_distribution/file/file_service.h"
#include "media_distribution/webrtc/webrtc_service.h"
#include "media_producer/media_manager.h"
#include <httplib.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ============================================================================
// 辅助函数
// ============================================================================

/**
 * @brief 生成 JSON 响应
 */
static std::string json_response(bool success, const std::string& message, 
                                  const json& data = nullptr) {
    json response;
    response["success"] = success;
    response["message"] = message;
    if (!data.is_null()) {
        response["data"] = data;
    }
    return response.dump();
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
        json data;
        
        // RTSP 状态
        data["rtsp"]["enabled"] = stream_config_.enable_rtsp;
        if (mgr && mgr->GetRtspService()) {
            data["rtsp"]["valid"] = mgr->GetRtspService()->IsValid();
            data["rtsp"]["running"] = mgr->GetRtspService()->IsRunning();
        }
        
        // WebRTC 状态
        data["webrtc"]["enabled"] = stream_config_.enable_webrtc;
        if (mgr && mgr->GetWebRTCService()) {
            data["webrtc"]["running"] = mgr->GetWebRTCService()->IsRunning();
        }
        
        // 录制状态
        data["recording"]["enabled"] = stream_config_.enable_file;
        if (mgr && mgr->GetFileService()) {
            auto* fs = mgr->GetFileService();
            data["recording"]["active"] = fs->IsRecording();
            data["recording"]["output_dir"] = stream_config_.mp4_config.outputDir;
        }
        
        // 媒体生产者状态
        auto& media_mgr = media::MediaManager::Instance();
        data["producer"]["mode"] = media::ProducerModeToString(media_mgr.GetCurrentMode());
        data["producer"]["running"] = media_mgr.IsRunning();
        
        res.set_content(json_response(true, "ok", data), "application/json");
    });

    // ========================================================================
    // RTSP 状态 API
    // ========================================================================
    server_->Get("/api/rtsp/status", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetRtspService()) {
            res.set_content(json_response(false, "RTSP not available"), "application/json");
            return;
        }
        
        auto* rtsp = mgr->GetRtspService();
        auto stats = rtsp->GetStats();
        json data;
        data["valid"] = rtsp->IsValid();
        data["running"] = rtsp->IsRunning();
        data["url"] = rtsp->GetUrl();
        data["frames_sent"] = stats.framesSent;
        data["bytes_sent"] = stats.bytesSent;
        res.set_content(json_response(true, "ok", data), "application/json");
    });

    // ========================================================================
    // RTSP 控制 API
    // ========================================================================
    server_->Post("/api/rtsp/start", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetRtspService()) {
            res.set_content(json_response(false, "RTSP not available"), "application/json");
            return;
        }
        
        auto* rtsp = mgr->GetRtspService();
        if (rtsp->IsRunning()) {
            res.set_content(json_response(true, "RTSP already running"), "application/json");
            return;
        }
        
        if (rtsp->Start()) {
            res.set_content(json_response(true, "RTSP started"), "application/json");
        } else {
            res.set_content(json_response(false, "Failed to start RTSP"), "application/json");
        }
    });

    server_->Post("/api/rtsp/stop", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetRtspService()) {
            res.set_content(json_response(false, "RTSP not available"), "application/json");
            return;
        }
        
        auto* rtsp = mgr->GetRtspService();
        if (!rtsp->IsRunning()) {
            res.set_content(json_response(true, "RTSP already stopped"), "application/json");
            return;
        }
        
        rtsp->Stop();
        res.set_content(json_response(true, "RTSP stopped"), "application/json");
    });

    // ========================================================================
    // WebRTC 状态和控制 API
    // ========================================================================
    server_->Get("/api/webrtc/status", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCService()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }
        
        auto* webrtc = mgr->GetWebRTCService();
        json data;
        data["running"] = webrtc->IsRunning();
        res.set_content(json_response(true, "ok", data), "application/json");
    });

    server_->Post("/api/webrtc/start", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCService()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }
        
        auto* webrtc = mgr->GetWebRTCService();
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
        if (!mgr || !mgr->GetWebRTCService()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }
        
        auto* webrtc = mgr->GetWebRTCService();
        if (!webrtc->IsRunning()) {
            res.set_content(json_response(true, "WebRTC already stopped"), "application/json");
            return;
        }
        
        webrtc->Stop();
        res.set_content(json_response(true, "WebRTC stopped"), "application/json");
    });

    // ========================================================================
    // 录制状态和控制 API
    // ========================================================================
    server_->Get("/api/record/status", [this](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetFileService()) {
            res.set_content(json_response(false, "Recording not available"), "application/json");
            return;
        }
        
        auto* fs = mgr->GetFileService();
        json data;
        data["enabled"] = stream_config_.enable_file;
        data["active"] = fs->IsRecording();
        data["output_dir"] = stream_config_.mp4_config.outputDir;
        
        if (fs->IsRecording()) {
            auto stats = fs->GetRecordStats();
            data["stats"]["frames_written"] = stats.frames_written;
            data["stats"]["bytes_written"] = stats.bytes_written;
            data["stats"]["duration_sec"] = stats.duration_sec;
        }
        
        res.set_content(json_response(true, "ok", data), "application/json");
    });

    server_->Post("/api/record/start", [this](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetFileService()) {
            res.set_content(json_response(false, "Recording not available"), "application/json");
            return;
        }
        
        auto* fs = mgr->GetFileService();
        if (fs->IsRecording()) {
            res.set_content(json_response(true, "Recording already active"), "application/json");
            return;
        }
        
        if (fs->StartRecording(stream_config_.mp4_config.outputDir)) {
            res.set_content(json_response(true, "Recording started"), "application/json");
        } else {
            res.set_content(json_response(false, "Failed to start recording"), "application/json");
        }
    });

    server_->Post("/api/record/stop", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetFileService()) {
            res.set_content(json_response(false, "Recording not available"), "application/json");
            return;
        }
        
        auto* fs = mgr->GetFileService();
        if (!fs->IsRecording()) {
            res.set_content(json_response(true, "Recording already stopped"), "application/json");
            return;
        }
        
        fs->StopRecording();
        res.set_content(json_response(true, "Recording stopped"), "application/json");
    });

    // ========================================================================
    // 生产者模式 API（基于新的 MediaManager）
    // ========================================================================
    server_->Get("/api/producer/status", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto& mgr = media::MediaManager::Instance();
        
        json data;
        data["mode"] = media::ProducerModeToString(mgr.GetCurrentMode());
        data["running"] = mgr.IsRunning();
        data["available_modes"] = json::array({"simple_ipc", "yolov5", "retinaface"});
        
        res.set_content(json_response(true, "ok", data), "application/json");
    });
    
    server_->Post("/api/producer/switch", [](const HttpRequest& req, HttpResponse& res) {
        try {
            json body = json::parse(req.body);
            std::string mode_str = body.value("mode", "simple_ipc");
            
            LOG_INFO("Producer mode switch requested: {}", mode_str);
            
            // 解析模式字符串
            media::ProducerMode target_mode;
            if (mode_str == "yolov5" || mode_str == "yolo") {
                target_mode = media::ProducerMode::YoloV5;
            } else if (mode_str == "retinaface" || mode_str == "face") {
                target_mode = media::ProducerMode::RetinaFace;
            } else {
                target_mode = media::ProducerMode::SimpleIPC;
            }
            
            auto& mgr = media::MediaManager::Instance();
            
            if (mgr.GetCurrentMode() == target_mode) {
                json data;
                data["mode"] = media::ProducerModeToString(target_mode);
                res.set_content(json_response(true, "Already in requested mode", data), "application/json");
                return;
            }
            
            // 切换模式（冷切换）
            if (!mgr.SwitchMode(target_mode)) {
                res.set_content(json_response(false, "Failed to switch producer mode"), "application/json");
                return;
            }
            
            json data;
            data["mode"] = media::ProducerModeToString(target_mode);
            res.set_content(json_response(true, "Producer mode switched", data), "application/json");
            
        } catch (const json::exception& e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });

    LOG_INFO("HTTP API 路由配置完成");
}
