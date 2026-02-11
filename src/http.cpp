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
#include "rtsp/rtsp_service.h"
#include "file/file_service.h"
#include "webrtc/webrtc_service.h"
#include "rknn/ai_engine.h"
#include "rknn/ai_service.h"
#include "rkvideo/pipeline_manager.h"
#include "rkvideo/ai_frame_processor.h"
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
    // WebRTC 控制 API
    // ========================================================================
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
    // WebRTC HTTP 信令 API（用于网页直连）
    // ========================================================================
    
    // 创建 Offer - 设备生成 SDP Offer
    server_->Post("/api/webrtc/offer", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCService()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }
        
        auto* webrtc = mgr->GetWebRTCService();
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
        if (!mgr || !mgr->GetWebRTCService()) {
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
            
            auto* webrtc = mgr->GetWebRTCService();
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
        if (!mgr || !mgr->GetWebRTCService()) {
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
            
            auto* webrtc = mgr->GetWebRTCService();
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
        if (!mgr || !mgr->GetWebRTCService()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }
        
        auto* webrtc = mgr->GetWebRTCService();
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
        if (!mgr || !mgr->GetFileService()) {
            res.set_content(json_response(false, "Recording not available"), "application/json");
            return;
        }
        
        auto* fs = mgr->GetFileService();
        json data;
        data["recording"] = fs->IsRecording();
        res.set_content(json_response(true, "ok", data), "application/json");
    });
    
    server_->Post("/api/record/start", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto* mgr = GetStreamManager();
        if (!mgr || !mgr->GetFileService()) {
            res.set_content(json_response(false, "Recording not available"), "application/json");
            return;
        }
        
        auto* fs = mgr->GetFileService();
        if (fs->IsRecording()) {
            res.set_content(json_response(true, "Recording already in progress"), "application/json");
            return;
        }
        
        fs->StartRecording();
        res.set_content(json_response(true, "Recording started"), "application/json");
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
    // AI 推理引擎 API
    // ========================================================================
    server_->Get("/api/ai/status", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto& engine = rknn::AIEngine::Instance();
        auto& service = rknn::AIService::Instance();
        
        json data;
        data["model_type"] = rknn::ModelTypeToString(engine.GetCurrentModelType());
        data["has_model"] = engine.HasModel();
        data["model_dir"] = engine.GetModelDir();
        data["service_running"] = service.IsRunning();
        
        // AI 服务统计
        auto stats = service.GetStats();
        data["stats"]["frames_processed"] = stats.frames_processed;
        data["stats"]["frames_skipped"] = stats.frames_skipped;
        data["stats"]["total_detections"] = stats.total_detections;
        data["stats"]["avg_inference_ms"] = stats.avg_inference_ms;
        
        if (engine.HasModel()) {
            auto info = engine.GetModelInfo();
            data["model_info"]["type"] = rknn::ModelTypeToString(info.type);
            data["model_info"]["input_width"] = info.input_width;
            data["model_info"]["input_height"] = info.input_height;
            data["model_info"]["input_channels"] = info.input_channels;
            data["model_info"]["is_quant"] = info.is_quant;
        }
        
        res.set_content(json_response(true, "ok", data), "application/json");
    });
    
    server_->Post("/api/ai/switch", [](const HttpRequest& req, HttpResponse& res) {
        try {
            json body = json::parse(req.body);
            std::string model_type_str = body.value("model", "none");
            
            rknn::ModelType type = rknn::StringToModelType(model_type_str);
            
            LOG_INFO("AI model switch requested: {}", model_type_str);
            
            auto& mgr = rkvideo::PipelineManager::Instance();
            auto current_mode = mgr.GetCurrentMode();
            
            // 确定目标管道模式
            rkvideo::PipelineMode target_mode = (type == rknn::ModelType::kNone) 
                ? rkvideo::PipelineMode::PureIPC 
                : rkvideo::PipelineMode::InferenceAI;
            
            // 如果需要切换管道模式
            if (current_mode != target_mode) {
                LOG_INFO("Pipeline mode switch: {} -> {}", 
                         current_mode == rkvideo::PipelineMode::PureIPC ? "PureIPC" : "InferenceAI",
                         target_mode == rkvideo::PipelineMode::PureIPC ? "PureIPC" : "InferenceAI");
                
                // 串行模式下停止 AIService（因为帧处理回调直接做 AI 推理）
                // 并行模式下可以启动 AIService（用于 OSD 叠加）
                if (target_mode == rkvideo::PipelineMode::InferenceAI) {
                    if (rknn::AIService::Instance().IsRunning()) {
                        LOG_INFO("Stopping AIService for serial mode (frame callback handles AI)");
                        rknn::AIService::Instance().Stop();
                    }
                }
                
                // 创建帧处理回调（仅用于串行模式）
                rkvideo::FrameProcessCallback callback = nullptr;
                if (target_mode == rkvideo::PipelineMode::InferenceAI) {
                    // 串行模式需要帧处理回调进行 AI 推理
                    callback = rkvideo::CreateAIFrameProcessCallback(640, 640);
                    if (!callback) {
                        LOG_ERROR("Failed to create AI frame processor");
                        res.set_content(json_response(false, "Failed to create AI frame processor"), "application/json");
                        return;
                    }
                }
                
                int switch_ret = mgr.SwitchMode(target_mode, callback);
                if (switch_ret != 0) {
                    LOG_ERROR("Failed to switch pipeline mode");
                    res.set_content(json_response(false, "Failed to switch pipeline mode"), "application/json");
                    return;
                }
            }
            
            // 切换 AI 模型
            int ret = rknn::AIEngine::Instance().SwitchModel(type);
            if (ret == 0) {
                json data;
                data["model_type"] = rknn::ModelTypeToString(type);
                data["pipeline_mode"] = target_mode == rkvideo::PipelineMode::PureIPC ? "parallel" : "serial";
                res.set_content(json_response(true, "Model and pipeline switched", data), "application/json");
            } else {
                res.set_content(json_response(false, "Failed to switch model"), "application/json");
            }
        } catch (const json::exception& e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });

    // ========================================================================
    // 管道模式切换 API
    // ========================================================================
    
    // GET /api/pipeline/status - 获取当前管道模式状态
    server_->Get("/api/pipeline/status", [](const HttpRequest& /*req*/, HttpResponse& res) {
        auto& mgr = rkvideo::PipelineManager::Instance();
        auto state = mgr.GetState();
        
        json data;
        
        // 如果 PipelineManager 未初始化，返回旧系统的状态
        if (!state.initialized) {
            data["mode"] = "parallel";
            data["initialized"] = false;
            data["streaming"] = true;  // rkvideo_init 后总是 streaming
            data["switch_count"] = 0;
            data["available_modes"] = json::array({"parallel"});
            data["resolution"] = {
                {"preset", "1080p"},
                {"width", 1920},
                {"height", 1080},
                {"framerate", 30}
            };
            data["available_resolutions"] = json::array();
            data["note"] = "Legacy rkvideo mode, dynamic switching not available";
            res.set_content(json_response(true, "ok", data), "application/json");
            return;
        }
        
        auto resolution = mgr.GetCurrentResolution();
        data["mode"] = state.mode == rkvideo::PipelineMode::PureIPC ? "parallel" : "serial";
        data["initialized"] = state.initialized;
        data["streaming"] = state.streaming;
        data["switch_count"] = state.mode_switch_count;
        data["available_modes"] = json::array({"parallel", "serial"});
        
        // 分辨率信息
        auto res_config = rkvideo::GetResolutionConfig(resolution);
        data["resolution"] = {
            {"preset", rkvideo::ResolutionPresetToString(resolution)},
            {"width", res_config.width},
            {"height", res_config.height},
            {"framerate", res_config.framerate}
        };
        data["available_resolutions"] = json::array({
            {{"preset", "1080p"}, {"width", 1920}, {"height", 1080}},
            {{"preset", "480p"}, {"width", 720}, {"height", 480}}
        });
        
        res.set_content(json_response(true, "ok", data), "application/json");
    });
    
    // POST /api/pipeline/switch - 切换管道模式
    // 请求体: {"mode": "serial"} 或 {"mode": "parallel"}
    server_->Post("/api/pipeline/switch", [](const HttpRequest& req, HttpResponse& res) {
        try {
            json body = json::parse(req.body);
            std::string mode_str = body.value("mode", "parallel");
            
            LOG_INFO("Pipeline mode switch requested: {}", mode_str);
            
            auto& mgr = rkvideo::PipelineManager::Instance();
            
            // 检查 PipelineManager 是否已初始化
            if (!mgr.GetState().initialized) {
                LOG_WARN("Pipeline mode switch unavailable: PipelineManager not initialized");
                res.set_content(json_response(false, "Mode switching not available in legacy mode"), "application/json");
                return;
            }
            
            rkvideo::PipelineMode target_mode;
            if (mode_str == "serial" || mode_str == "inference") {
                target_mode = rkvideo::PipelineMode::InferenceAI;
            } else {
                target_mode = rkvideo::PipelineMode::PureIPC;
            }
            
            // 创建帧处理回调（仅用于串行模式）
            rkvideo::FrameProcessCallback callback = nullptr;
            if (target_mode == rkvideo::PipelineMode::InferenceAI) {
                callback = rkvideo::CreateAIFrameProcessCallback(640, 640);
                if (!callback) {
                    res.set_content(json_response(false, "Failed to create AI frame processor"), "application/json");
                    return;
                }
            }
            
            // 切换模式（冷启动）
            int ret = mgr.SwitchMode(target_mode, callback);
            
            if (ret != 0) {
                res.set_content(json_response(false, "Mode switch failed"), "application/json");
                return;
            }
            
            auto state = mgr.GetState();
            json data;
            data["mode"] = state.mode == rkvideo::PipelineMode::PureIPC ? "parallel" : "serial";
            data["switch_count"] = state.mode_switch_count;
            
            // 如果切换到推理模式，强制 480p
            if (target_mode == rkvideo::PipelineMode::InferenceAI) {
                mgr.SetResolution(rkvideo::ResolutionPreset::R_480P);
                data["note"] = "InferenceAI mode forces 480p resolution";
            }
            
            res.set_content(json_response(true, "Mode switched", data), "application/json");
        } catch (const json::exception& e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });
    
    // POST /api/pipeline/resolution - 切换分辨率（冷切换）
    // 请求体: {"resolution": "1080p"} 或 {"resolution": "480p"}
    server_->Post("/api/pipeline/resolution", [](const HttpRequest& req, HttpResponse& res) {
        try {
            json body = json::parse(req.body);
            std::string resolution_str = body.value("resolution", "1080p");
            
            LOG_INFO("Resolution switch requested: {}", resolution_str);
            
            auto& mgr = rkvideo::PipelineManager::Instance();
            
            // 检查 PipelineManager 是否已初始化
            if (!mgr.GetState().initialized) {
                LOG_WARN("Resolution switch unavailable: PipelineManager not initialized");
                res.set_content(json_response(false, "Resolution switching not available in legacy mode"), "application/json");
                return;
            }
            
            // 检查是否在推理模式
            if (mgr.GetCurrentMode() == rkvideo::PipelineMode::InferenceAI) {
                json data;
                data["resolution"] = "480p";
                data["note"] = "InferenceAI mode forces 480p, cannot change resolution";
                res.set_content(json_response(false, "Resolution locked in inference mode", data), "application/json");
                return;
            }
            
            rkvideo::ResolutionPreset preset = rkvideo::StringToResolutionPreset(resolution_str);
            
            // 切换分辨率（冷启动）
            int ret = mgr.SetResolution(preset);
            
            if (ret != 0) {
                res.set_content(json_response(false, "Resolution switch failed"), "application/json");
                return;
            }
            
            auto res_config = rkvideo::GetResolutionConfig(preset);
            json data;
            data["resolution"] = rkvideo::ResolutionPresetToString(preset);
            data["width"] = res_config.width;
            data["height"] = res_config.height;
            data["framerate"] = res_config.framerate;
            
            res.set_content(json_response(true, "Resolution switched (cold start)", data), "application/json");
        } catch (const json::exception& e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });
    
    // POST /api/pipeline/framerate - 切换帧率（冷切换）
    // 请求体: {"framerate": 30} (1-30)
    server_->Post("/api/pipeline/framerate", [](const HttpRequest& req, HttpResponse& res) {
        try {
            json body = json::parse(req.body);
            int fps = body.value("framerate", 30);
            
            LOG_INFO("Frame rate switch requested: {}", fps);
            
            auto& mgr = rkvideo::PipelineManager::Instance();
            
            // 检查 PipelineManager 是否已初始化
            if (!mgr.GetState().initialized) {
                LOG_WARN("Frame rate switch unavailable: PipelineManager not initialized");
                res.set_content(json_response(false, "Frame rate switching not available in legacy mode"), "application/json");
                return;
            }
            
            // 限制帧率范围
            fps = std::max(1, std::min(fps, 30));
            
            // 切换帧率（冷启动）
            int ret = mgr.SetFrameRate(fps);
            
            if (ret != 0) {
                res.set_content(json_response(false, "Frame rate switch failed"), "application/json");
                return;
            }
            
            json data;
            data["framerate"] = fps;
            
            res.set_content(json_response(true, "Frame rate switched (cold start)", data), "application/json");
        } catch (const json::exception& e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });

    LOG_INFO("HTTP API 路由配置完成");
}
