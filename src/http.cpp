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
#include "webrtc/thread_webrtc.h"
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
        if (mgr && mgr->GetRtspThread()) {
            data["rtsp"]["valid"] = mgr->GetRtspThread()->IsValid();
        }
        
        // WebRTC 状态
        data["webrtc"]["enabled"] = stream_config_.enable_webrtc;
        if (mgr && mgr->GetWebRTCThread()) {
            data["webrtc"]["running"] = mgr->GetWebRTCThread()->IsRunning();
        }
        
        // 录制状态
        data["recording"]["enabled"] = stream_config_.enable_file;
        if (mgr && mgr->GetFileThread()) {
            auto* ft = mgr->GetFileThread();
            data["recording"]["active"] = ft->IsRecording();
            data["recording"]["output_dir"] = stream_config_.mp4_config.outputDir;
        }
        
        res.set_content(json_response(true, "ok", data), "application/json");
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
        json data;
        data["valid"] = rtsp->IsValid();
        data["url"] = rtsp->GetUrl();
        data["frames_sent"] = stats.framesSent;
        data["bytes_sent"] = stats.bytesSent;
        res.set_content(json_response(true, "ok", data), "application/json");
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
    // WebRTC HTTP 信令 API（用于网页直连）
    // ========================================================================
    
    // 创建 Offer - 设备生成 SDP Offer
    server_->Post("/api/webrtc/offer", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCThread()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }
        
        auto* webrtc = mgr->GetWebRTCThread();
        std::string offer = webrtc->CreateOfferForHttp();
        
        if (offer.empty()) {
            res.set_content(json_response(false, "Failed to create offer"), "application/json");
            return;
        }
        
        json data;
        data["sdp"] = offer;  // nlohmann/json 自动处理转义
        data["type"] = "offer";
        res.set_content(json_response(true, "ok", data), "application/json");
    });
    
    // 设置 Answer - 接收客户端的 SDP Answer
    server_->Post("/api/webrtc/answer", [](const HttpRequest& req, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCThread()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }
        
        try {
            json body = json::parse(req.body);
            std::string sdp = body.value("sdp", "");
            
            if (sdp.empty()) {
                res.set_content(json_response(false, "Missing sdp field"), "application/json");
                return;
            }
            
            auto* webrtc = mgr->GetWebRTCThread();
            if (webrtc->SetAnswerFromHttp(sdp)) {
                res.set_content(json_response(true, "Answer set"), "application/json");
            } else {
                res.set_content(json_response(false, "Failed to set answer"), "application/json");
            }
        } catch (const json::exception& e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });
    
    // 添加 ICE 候选
    server_->Post("/api/webrtc/ice", [](const HttpRequest& req, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCThread()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }
        
        try {
            json body = json::parse(req.body);
            std::string candidate = body.value("candidate", "");
            std::string mid = body.value("sdpMid", "");
            
            if (candidate.empty()) {
                // 可能是 ICE 收集完成信号
                res.set_content(json_response(true, "ICE gathering complete"), "application/json");
                return;
            }
            
            auto* webrtc = mgr->GetWebRTCThread();
            if (webrtc->AddIceCandidateFromHttp(candidate, mid)) {
                res.set_content(json_response(true, "ICE candidate added"), "application/json");
            } else {
                res.set_content(json_response(false, "Failed to add ICE candidate"), "application/json");
            }
        } catch (const json::exception& e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });
    
    // 获取本地 ICE 候选列表
    server_->Get("/api/webrtc/ice", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCThread()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }
        
        auto* webrtc = mgr->GetWebRTCThread();
        auto candidates = webrtc->GetLocalIceCandidates();
        
        json data = json::array();
        for (const auto& [candidate, mid] : candidates) {
            data.push_back({{"candidate", candidate}, {"sdpMid", mid}});
        }
        
        res.set_content(json_response(true, "ok", data), "application/json");
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
        json data;
        data["recording"] = ft->IsRecording();
        res.set_content(json_response(true, "ok", data), "application/json");
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
