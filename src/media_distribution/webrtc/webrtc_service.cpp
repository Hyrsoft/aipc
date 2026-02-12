/**
 * @file webrtc_service.cpp
 * @brief WebRTC 推流服务实现
 *
 * @author 好软，好温暖
 * @date 2026-02-04
 */

#include "webrtc_service.h"
#include "common/logger.h"

#undef LOG_TAG
#define LOG_TAG "webrtc_service"

// ============================================================================
// 全局实例
// ============================================================================

static std::unique_ptr<WebRTCService> g_webrtc_service;

// ============================================================================
// WebRTCService 实现
// ============================================================================

WebRTCService::WebRTCService(const WebRTCServiceConfig& config)
    : config_(config)
{
    LOG_INFO("WebRTC 服务创建: device_id={}", config_.device_id);
}

WebRTCService::~WebRTCService() {
    Stop();
    LOG_INFO("WebRTC 服务销毁");
}

bool WebRTCService::Start() {
    if (valid_) {
        LOG_WARN("WebRTC 服务已启动");
        return true;
    }

    // 创建信令客户端
    SignalingConfig sig_config;
    sig_config.device_id = config_.device_id;
    sig_config.server_url = config_.signaling_url;
    sig_config.auto_reconnect = true;
    sig_config.reconnect_interval_ms = 5000;

    signaling_ = std::make_shared<SignalingClient>(sig_config);

    // 创建 WebRTC 系统
    webrtc_ = std::make_shared<WebRTCSystem>(config_.webrtc_config);

    // 初始化 WebRTC 系统
    auto err = webrtc_->Init(signaling_);
    if (err != WebRTCError::kNone) {
        LOG_ERROR("WebRTC 系统初始化失败");
        return false;
    }

    // 连接信令服务器
    if (!signaling_->Connect()) {
        LOG_ERROR("连接信令服务器失败");
        return false;
    }

    // 设置信令状态回调
    signaling_->OnStatusChanged([this](SignalingStatus status) {
        LOG_INFO("信令状态: {}", SignalingClient::StatusToString(status));
        
        // 自动加入房间
        if (status == SignalingStatus::kConnected) {
            signaling_->JoinRoom();
        }
    });

    valid_ = true;
    LOG_INFO("WebRTC 服务启动成功");
    return true;
}

void WebRTCService::Stop() {
    if (!valid_) {
        return;
    }

    valid_ = false;

    if (webrtc_) {
        webrtc_->Deinit();
        webrtc_.reset();
    }

    if (signaling_) {
        signaling_->Disconnect();
        signaling_.reset();
    }

    LOG_INFO("WebRTC 服务已停止");
}

bool WebRTCService::IsConnected() const {
    return webrtc_ && webrtc_->IsConnected();
}

WebRTCState WebRTCService::GetState() const {
    return webrtc_ ? webrtc_->GetState() : WebRTCState::kIdle;
}

WebRTCStats WebRTCService::GetStats() const {
    return webrtc_ ? webrtc_->GetStats() : WebRTCStats{};
}

void WebRTCService::StreamConsumer(EncodedStreamPtr stream, void* user_data) {
    auto* self = static_cast<WebRTCService*>(user_data);
    if (self) {
        self->SendVideoFrame(stream);
    }
}

void WebRTCService::SendVideoFrame(const EncodedStreamPtr& stream) {
    if (!IsConnected() || !stream || !stream->pstPack) {
        return;
    }

    // 获取视频数据
    const uint8_t* data = static_cast<const uint8_t*>(
        RK_MPI_MB_Handle2VirAddr(stream->pstPack->pMbBlk));
    uint32_t len = stream->pstPack->u32Len;
    uint64_t pts = stream->pstPack->u64PTS;

    if (data && len > 0) {
        webrtc_->SendVideoData(data, len, pts);
    }
}

void WebRTCService::OnStateChanged(StateCallback callback) {
    if (webrtc_) {
        webrtc_->OnStateChanged(std::move(callback));
    }
}

void WebRTCService::OnError(ErrorCallback callback) {
    if (webrtc_) {
        webrtc_->OnError(std::move(callback));
    }
}

// ============================================================================
// HTTP 信令模式实现
// ============================================================================

std::string WebRTCService::CreateOfferForHttp() {
    if (!webrtc_) {
        LOG_ERROR("WebRTC 系统未初始化");
        return "";
    }
    return webrtc_->CreateOfferForHttp();
}

bool WebRTCService::SetAnswerFromHttp(const std::string& sdp) {
    if (!webrtc_) {
        LOG_ERROR("WebRTC 系统未初始化");
        return false;
    }
    return webrtc_->SetAnswerFromHttp(sdp);
}

bool WebRTCService::AddIceCandidateFromHttp(const std::string& candidate, const std::string& mid) {
    if (!webrtc_) {
        LOG_ERROR("WebRTC 系统未初始化");
        return false;
    }
    return webrtc_->AddIceCandidateFromHttp(candidate, mid);
}

std::vector<std::pair<std::string, std::string>> WebRTCService::GetLocalIceCandidates() {
    if (!webrtc_) {
        return {};
    }
    return webrtc_->GetLocalIceCandidates();
}

// ============================================================================
// 全局实例管理实现
// ============================================================================

WebRTCService* GetWebRTCService() {
    return g_webrtc_service.get();
}

void CreateWebRTCService(const WebRTCServiceConfig& config) {
    if (g_webrtc_service) {
        LOG_WARN("WebRTC 服务实例已存在");
        return;
    }
    g_webrtc_service = std::make_unique<WebRTCService>(config);
}

void DestroyWebRTCService() {
    g_webrtc_service.reset();
}

