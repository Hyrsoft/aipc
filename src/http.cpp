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
#include "media_producer/visiong/visiong_producer.h"
#include "media_producer/visiong/python/python_strategy.h"
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;

// ============================================================================
// 模型文件目录（上传和管理）
// ============================================================================
static const std::string MODEL_DIR = "../model";
static const size_t MAX_UPLOAD_SIZE = 50 * 1024 * 1024;  // 50MB

// ============================================================================
// 辅助函数
// ============================================================================

/**
 * @brief 获取当前 VisionGProducer 下的 PythonStrategy（如果处于 Python 模式）
 */
static media::PythonStrategy* GetPythonStrategy() {
    auto& mgr = media::MediaManager::Instance();
    if (mgr.GetCurrentMode() != media::ProducerMode::VisionG ||
        mgr.GetCurrentVisionGModel() != media::VisionGModel::Python) {
        return nullptr;
    }
    auto* producer = dynamic_cast<media::VisionGProducer*>(mgr.GetProducer());
    if (!producer) return nullptr;
    return dynamic_cast<media::PythonStrategy*>(producer->GetStrategy());
}

/**
 * @brief 净化文件名，移除路径穿越字符
 */
static std::string SanitizeFilename(const std::string& name) {
    std::string result;
    for (char c : name) {
        if (c == '/' || c == '\\' || c == '\0') continue;
        result += c;
    }
    // 防止 ".." 路径穿越
    if (result.find("..") != std::string::npos) {
        return "";
    }
    return result;
}

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
    // WebRTC HTTP 信令 API
    // ========================================================================
    server_->Post("/api/webrtc/offer", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCService()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }
        
        auto* webrtc = mgr->GetWebRTCService();
        if (!webrtc->IsRunning()) {
            res.set_content(json_response(false, "WebRTC service not running"), "application/json");
            return;
        }
        
        try {
            std::string offer = webrtc->CreateOfferForHttp();
            if (offer.empty()) {
                res.set_content(json_response(false, "Failed to create offer"), "application/json");
                return;
            }
            
            json data;
            data["sdp"] = offer;
            data["type"] = "offer";
            res.set_content(json_response(true, "ok", data), "application/json");
        } catch (const std::exception& e) {
            res.set_content(json_response(false, std::string("Error: ") + e.what()), "application/json");
        }
    });
    
    server_->Post("/api/webrtc/answer", [](const HttpRequest& req, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCService()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }
        
        auto* webrtc = mgr->GetWebRTCService();
        
        try {
            json body = json::parse(req.body);
            std::string sdp = body.value("sdp", "");
            
            if (sdp.empty()) {
                res.set_content(json_response(false, "Missing SDP"), "application/json");
                return;
            }
            
            if (webrtc->SetAnswerFromHttp(sdp)) {
                res.set_content(json_response(true, "Answer set"), "application/json");
            } else {
                res.set_content(json_response(false, "Failed to set answer"), "application/json");
            }
        } catch (const json::exception& e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });
    
    server_->Post("/api/webrtc/ice", [](const HttpRequest& req, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCService()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }
        
        auto* webrtc = mgr->GetWebRTCService();
        
        try {
            json body = json::parse(req.body);
            std::string candidate = body.value("candidate", "");
            std::string mid = body.value("sdpMid", "0");
            
            if (candidate.empty()) {
                // 空候选表示 ICE 收集完成
                res.set_content(json_response(true, "ICE gathering complete"), "application/json");
                return;
            }
            
            if (webrtc->AddIceCandidateFromHttp(candidate, mid)) {
                res.set_content(json_response(true, "ICE candidate added"), "application/json");
            } else {
                res.set_content(json_response(false, "Failed to add ICE candidate"), "application/json");
            }
        } catch (const json::exception& e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });
    
    server_->Get("/api/webrtc/candidates", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCService()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }
        
        auto* webrtc = mgr->GetWebRTCService();
        auto candidates = webrtc->GetLocalIceCandidates();
        
        json data = json::array();
        for (const auto& [candidate, mid] : candidates) {
            json c;
            c["candidate"] = candidate;
            c["sdpMid"] = mid;
            data.push_back(c);
        }
        
        res.set_content(json_response(true, "ok", data), "application/json");
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
        
        if (fs->StartRecording()) {
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
        data["available_modes"] = json::array({"simple_ipc", "visiong"});
        
        res.set_content(json_response(true, "ok", data), "application/json");
    });
    
    server_->Post("/api/producer/switch", [](const HttpRequest& req, HttpResponse& res) {
        try {
            json body = json::parse(req.body);
            std::string mode_str = body.value("mode", "simple_ipc");
            
            LOG_INFO("Producer mode switch requested: {}", mode_str);
            
            auto& mgr = media::MediaManager::Instance();

            // 解析模式字符串
            media::ProducerMode target_mode;
            if (mode_str == "python") {
                target_mode = media::ProducerMode::VisionG;
                mgr.SetVisionGModel(media::VisionGModel::Python);
            } else if (mode_str == "visiong" || mode_str == "yolov5" || mode_str == "yolo") {
                target_mode = media::ProducerMode::VisionG;
                mgr.SetVisionGModel(media::VisionGModel::YOLOv5);
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
            if (mgr.SwitchMode(target_mode) != 0) {
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

    // ========================================================================
    // AI API（前端兼容接口）
    // ========================================================================
    server_->Get("/api/ai/status", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto& mgr = media::MediaManager::Instance();
        auto mode = mgr.GetCurrentMode();
        
        json data;
        // has_model: 是否加载了 AI 模型
        bool has_model = (mode == media::ProducerMode::VisionG);
        data["has_model"] = has_model;
        
        // model_type: 模型类型名称
        if (mode == media::ProducerMode::VisionG) {
            data["model_type"] = media::VisionGModelToString(mgr.GetCurrentVisionGModel());
        } else {
            data["model_type"] = "none";
        }
        
        // stats: AI 统计信息（当前未实现详细统计，返回占位数据）
        json stats;
        stats["frames_processed"] = 0;
        stats["avg_inference_ms"] = 0;
        stats["total_detections"] = 0;
        data["stats"] = stats;
        
        res.set_content(json_response(true, "ok", data), "application/json");
    });
    
    server_->Post("/api/ai/switch", [](const HttpRequest& req, HttpResponse& res) {
        try {
            json body = json::parse(req.body);
            std::string model_str = body.value("model", "none");
            
            LOG_INFO("AI model switch requested: {}", model_str);
            
            auto& mgr = media::MediaManager::Instance();

            // 映射模型名称到生产者模式
            media::ProducerMode target_mode;
            if (model_str == "python") {
                target_mode = media::ProducerMode::VisionG;
                mgr.SetVisionGModel(media::VisionGModel::Python);
            } else if (model_str == "yolov5" || model_str == "yolo" || model_str == "visiong") {
                target_mode = media::ProducerMode::VisionG;
                mgr.SetVisionGModel(media::VisionGModel::YOLOv5);
            } else {
                target_mode = media::ProducerMode::SimpleIPC;
            }
            
            auto& mgr = media::MediaManager::Instance();
            
            if (mgr.GetCurrentMode() == target_mode) {
                json data;
                data["model"] = model_str;
                res.set_content(json_response(true, "Already using requested model", data), "application/json");
                return;
            }
            
            // 切换模式（冷切换）
            if (mgr.SwitchMode(target_mode) != 0) {
                res.set_content(json_response(false, "Failed to switch AI model"), "application/json");
                return;
            }
            
            json data;
            data["model"] = model_str;
            res.set_content(json_response(true, "AI model switched", data), "application/json");
            
        } catch (const json::exception& e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });

    // ========================================================================
    // Pipeline API（前端兼容接口）
    // ========================================================================
    server_->Get("/api/pipeline/status", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto& mgr = media::MediaManager::Instance();
        auto mode = mgr.GetCurrentMode();
        auto cfg = mgr.GetConfig();
        auto res_cfg = cfg.GetResolutionConfig();
        
        json data;
        
        // mode: parallel (纯 IPC) / serial (AI 推理)
        if (mode == media::ProducerMode::SimpleIPC) {
            data["mode"] = "parallel";
        } else {
            data["mode"] = "serial";
        }
        
        // resolution 信息
        json resolution;
        if (cfg.resolution == media::Resolution::R_1080P) {
            resolution["preset"] = "1080p";
        } else if (cfg.resolution == media::Resolution::R_720P) {
            resolution["preset"] = "720p";
        } else {
            resolution["preset"] = "480p";
        }
        resolution["width"] = res_cfg.width;
        resolution["height"] = res_cfg.height;
        resolution["framerate"] = res_cfg.framerate;
        data["resolution"] = resolution;
        
        // 状态信息
        data["initialized"] = mgr.IsInitialized();
        data["streaming"] = mgr.IsRunning();
        
        // 可用分辨率
        if (mode == media::ProducerMode::SimpleIPC) {
            data["available_resolutions"] = json::array({"1080p", "720p", "480p"});
            data["note"] = "";
        } else {
            // AI 模式下只支持 480p
            data["available_resolutions"] = json::array({"480p"});
            data["note"] = "AI inference mode only supports 480p";
        }
        
        res.set_content(json_response(true, "ok", data), "application/json");
    });
    
    server_->Post("/api/pipeline/resolution", [](const HttpRequest& req, HttpResponse& res) {
        try {
            json body = json::parse(req.body);
            std::string preset_str = body.value("resolution", "1080p");
            
            LOG_INFO("Resolution switch requested: {}", preset_str);
            
            // 解析分辨率预设
            media::Resolution target_res;
            if (preset_str == "720p") {
                target_res = media::Resolution::R_720P;
            } else if (preset_str == "480p") {
                target_res = media::Resolution::R_480P;
            } else {
                target_res = media::Resolution::R_1080P;
            }
            
            auto& mgr = media::MediaManager::Instance();
            
            // AI 模式下只支持 480p
            if (mgr.GetCurrentMode() != media::ProducerMode::SimpleIPC && 
                target_res != media::Resolution::R_480P) {
                res.set_content(json_response(false, "AI mode only supports 480p"), "application/json");
                return;
            }
            
            if (mgr.GetConfig().resolution == target_res) {
                auto res_cfg = media::ResolutionConfig::FromPreset(target_res);
                json data;
                data["resolution"] = preset_str;
                data["width"] = res_cfg.width;
                data["height"] = res_cfg.height;
                res.set_content(json_response(true, "Already using requested resolution", data), "application/json");
                return;
            }
            
            // 切换分辨率（需要重新初始化）
            if (mgr.SetResolution(target_res) != 0) {
                res.set_content(json_response(false, "Failed to switch resolution"), "application/json");
                return;
            }
            
            auto res_cfg = media::ResolutionConfig::FromPreset(target_res);
            json data;
            data["resolution"] = preset_str;
            data["width"] = res_cfg.width;
            data["height"] = res_cfg.height;
            res.set_content(json_response(true, "Resolution switched", data), "application/json");
            
        } catch (const json::exception& e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });

    // ========================================================================
    // 模型文件管理 API
    // ========================================================================

    // 列出所有模型文件
    server_->Get("/api/model/list", [](const HttpRequest& /*req*/, HttpResponse& res) {
        namespace fs = std::filesystem;
        json models = json::array();

        try {
            for (const auto& entry : fs::directory_iterator(MODEL_DIR)) {
                if (!entry.is_regular_file()) continue;
                auto ext = entry.path().extension().string();
                // 小写化扩展名
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".rknn" || ext == ".txt") {
                    json item;
                    item["name"] = entry.path().filename().string();
                    item["size"] = entry.file_size();
                    item["type"] = ext.substr(1);  // "rknn" or "txt"
                    models.push_back(item);
                }
            }
        } catch (const fs::filesystem_error& e) {
            res.set_content(json_response(false, std::string("Failed to list models: ") + e.what()), "application/json");
            return;
        }

        res.set_content(json_response(true, "ok", models), "application/json");
    });

    // 上传模型文件
    server_->Post("/api/model/upload", [](const HttpRequest& req, HttpResponse& res) {
        if (!req.has_file("file")) {
            res.set_content(json_response(false, "No file in request"), "application/json");
            return;
        }

        const auto& file = req.get_file_value("file");
        std::string filename = SanitizeFilename(file.filename);

        if (filename.empty()) {
            res.set_content(json_response(false, "Invalid filename"), "application/json");
            return;
        }

        // 仅允许 .rknn 和 .txt 后缀
        std::string ext = filename.substr(filename.find_last_of('.') + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != "rknn" && ext != "txt") {
            res.set_content(json_response(false, "Only .rknn and .txt files are allowed"), "application/json");
            return;
        }

        // 文件大小限制
        if (file.content.size() > MAX_UPLOAD_SIZE) {
            res.set_content(json_response(false, "File too large (max 50MB)"), "application/json");
            return;
        }

        // 禁止覆盖内置模型
        if (filename == "yolov5.rknn") {
            res.set_content(json_response(false, "Cannot overwrite built-in model"), "application/json");
            return;
        }

        std::string path = MODEL_DIR + "/" + filename;
        std::ofstream ofs(path, std::ios::binary);
        if (!ofs) {
            res.set_content(json_response(false, "Failed to write file"), "application/json");
            return;
        }
        ofs.write(file.content.data(), static_cast<std::streamsize>(file.content.size()));
        ofs.close();

        json data;
        data["name"] = filename;
        data["size"] = file.content.size();
        LOG_INFO("Model file uploaded: {} ({} bytes)", filename, file.content.size());
        res.set_content(json_response(true, "File uploaded", data), "application/json");
    });

    // 删除模型文件
    server_->Delete(R"(/api/model/(.+))", [](const HttpRequest& req, HttpResponse& res) {
        std::string filename = SanitizeFilename(req.matches[1].str());

        if (filename.empty()) {
            res.set_content(json_response(false, "Invalid filename"), "application/json");
            return;
        }

        // 禁止删除内置模型
        if (filename == "yolov5.rknn" || filename == "coco_80_labels_list.txt") {
            res.set_content(json_response(false, "Cannot delete built-in files"), "application/json");
            return;
        }

        std::string path = MODEL_DIR + "/" + filename;
        if (!std::filesystem::exists(path)) {
            res.set_content(json_response(false, "File not found"), "application/json");
            return;
        }

        std::filesystem::remove(path);
        LOG_INFO("Model file deleted: {}", filename);
        res.set_content(json_response(true, "File deleted"), "application/json");
    });

    // ========================================================================
    // Python 编辑器 API
    // ========================================================================

    // 获取当前 Python 代码
    server_->Get("/api/python/code", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* strategy = GetPythonStrategy();
        if (!strategy) {
            res.set_content(json_response(false, "Not in Python mode"), "application/json");
            return;
        }

        json data;
        data["code"] = strategy->GetCurrentCode();
        res.set_content(json_response(true, "ok", data), "application/json");
    });

    // 提交 / 热更新 Python 代码
    server_->Post("/api/python/code", [](const HttpRequest& req, HttpResponse& res) {
        auto* strategy = GetPythonStrategy();
        if (!strategy) {
            res.set_content(json_response(false, "Not in Python mode"), "application/json");
            return;
        }

        try {
            json body = json::parse(req.body);
            std::string code = body.value("code", "");
            if (code.empty()) {
                res.set_content(json_response(false, "Empty code"), "application/json");
                return;
            }

            std::string err = strategy->UpdateCode(code);
            if (err.empty()) {
                res.set_content(json_response(true, "Code updated"), "application/json");
            } else {
                json data;
                data["error"] = err;
                res.set_content(json_response(false, "Code error", data), "application/json");
            }
        } catch (const json::exception& e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });

    // Python 模式状态
    server_->Get("/api/python/status", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto& mgr = media::MediaManager::Instance();
        bool is_python = (mgr.GetCurrentMode() == media::ProducerMode::VisionG &&
                          mgr.GetCurrentVisionGModel() == media::VisionGModel::Python);

        json data;
        data["active"] = is_python;

        if (is_python) {
            auto* strategy = GetPythonStrategy();
            if (strategy) {
                data["last_error"] = strategy->GetLastError();
                auto info = strategy->GetModelInfo();
                data["model"]["path"] = info.model_path;
                data["model"]["label_path"] = info.label_path;
                data["model"]["type"] = info.model_type;
            }
        }

        res.set_content(json_response(true, "ok", data), "application/json");
    });

    // 切换 NPU 模型（Python 模式下）
    server_->Post("/api/python/model", [](const HttpRequest& req, HttpResponse& res) {
        try {
            json body = json::parse(req.body);
            std::string model_path = body.value("model_path", "");
            std::string label_path = body.value("label_path", "");
            std::string model_type = body.value("model_type", "YOLOV5");

            if (model_path.empty()) {
                res.set_content(json_response(false, "model_path is required"), "application/json");
                return;
            }

            // 净化路径：仅允许纯文件名
            model_path = SanitizeFilename(model_path);
            label_path = SanitizeFilename(label_path);
            if (model_path.empty()) {
                res.set_content(json_response(false, "Invalid model_path"), "application/json");
                return;
            }

            // 检查文件存在
            std::string full_model = MODEL_DIR + "/" + model_path;
            if (!std::filesystem::exists(full_model)) {
                res.set_content(json_response(false, "Model file not found: " + model_path), "application/json");
                return;
            }

            auto* strategy = GetPythonStrategy();
            if (!strategy) {
                res.set_content(json_response(false, "Not in Python mode"), "application/json");
                return;
            }

            auto* producer = dynamic_cast<media::VisionGProducer*>(
                media::MediaManager::Instance().GetProducer());
            if (!producer) {
                res.set_content(json_response(false, "Producer not available"), "application/json");
                return;
            }

            // 暂停帧循环 → 重建 NPU → 恢复帧循环
            producer->PauseFrameLoop();

            std::string full_label = label_path.empty() ? "" : (MODEL_DIR + "/" + label_path);
            auto new_npu = strategy->UpdateModel(full_model, full_label, model_type);
            if (!new_npu) {
                producer->ResumeFrameLoop();
                res.set_content(json_response(false, "Failed to load model"), "application/json");
                return;
            }

            producer->ReplaceNPU(std::move(new_npu));
            producer->ResumeFrameLoop();

            json data;
            data["model_path"] = model_path;
            data["model_type"] = model_type;
            LOG_INFO("Python mode model switched: {} ({})", model_path, model_type);
            res.set_content(json_response(true, "Model switched", data), "application/json");

        } catch (const json::exception& e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });

    // 获取 Python 模板列表
    server_->Get("/api/python/templates", [](const HttpRequest& /*req*/, HttpResponse& res) {
        namespace fs = std::filesystem;
        json templates = json::array();
        std::string template_dir = "../python_templates";

        try {
            if (fs::exists(template_dir)) {
                for (const auto& entry : fs::directory_iterator(template_dir)) {
                    if (!entry.is_regular_file()) continue;
                    auto ext = entry.path().extension().string();
                    if (ext != ".py") continue;

                    std::ifstream ifs(entry.path());
                    std::string content((std::istreambuf_iterator<char>(ifs)),
                                        std::istreambuf_iterator<char>());

                    json tmpl;
                    tmpl["name"] = entry.path().stem().string();
                    tmpl["filename"] = entry.path().filename().string();
                    tmpl["code"] = content;
                    templates.push_back(tmpl);
                }
            }
        } catch (const fs::filesystem_error& e) {
            LOG_WARN("Failed to list templates: {}", e.what());
        }

        res.set_content(json_response(true, "ok", templates), "application/json");
    });

    LOG_INFO("HTTP API 路由配置完成");
}
