/**
 * @file thread_webrtc.cpp
 * @brief WebRTC 推流线程实现
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#include "thread_webrtc.h"
#include "common/logger.h"

#undef LOG_TAG
#define LOG_TAG "webrtc_thread"

// ============================================================================
// 全局实例
// ============================================================================

static std::unique_ptr<WebRTCThread> g_webrtc_thread;

// ============================================================================
// WebRTCThread 实现
// ============================================================================

WebRTCThread::WebRTCThread(const WebRTCThreadConfig& config)
    : config_(config)
{
    LOG_INFO("WebRTC 线程创建: device_id={}", config_.device_id);
}

WebRTCThread::~WebRTCThread() {
    Stop();
    LOG_INFO("WebRTC 线程销毁");
}

bool WebRTCThread::Start() {
    if (valid_) {
        LOG_WARN("WebRTC 线程已启动");
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
    LOG_INFO("WebRTC 线程启动成功");
    return true;
}

void WebRTCThread::Stop() {
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

    LOG_INFO("WebRTC 线程已停止");
}

bool WebRTCThread::IsConnected() const {
    return webrtc_ && webrtc_->IsConnected();
}

WebRTCState WebRTCThread::GetState() const {
    return webrtc_ ? webrtc_->GetState() : WebRTCState::kIdle;
}

WebRTCStats WebRTCThread::GetStats() const {
    return webrtc_ ? webrtc_->GetStats() : WebRTCStats{};
}

void WebRTCThread::StreamConsumer(EncodedStreamPtr stream, void* user_data) {
    auto* self = static_cast<WebRTCThread*>(user_data);
    if (self) {
        self->SendVideoFrame(stream);
    }
}

void WebRTCThread::SendVideoFrame(const EncodedStreamPtr& stream) {
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

void WebRTCThread::OnStateChanged(StateCallback callback) {
    if (webrtc_) {
        webrtc_->OnStateChanged(std::move(callback));
    }
}

void WebRTCThread::OnError(ErrorCallback callback) {
    if (webrtc_) {
        webrtc_->OnError(std::move(callback));
    }
}

// ============================================================================
// 全局实例管理实现
// ============================================================================

WebRTCThread* GetWebRTCThread() {
    return g_webrtc_thread.get();
}

void CreateWebRTCThread(const WebRTCThreadConfig& config) {
    if (g_webrtc_thread) {
        LOG_WARN("WebRTC 线程实例已存在");
        return;
    }
    g_webrtc_thread = std::make_unique<WebRTCThread>(config);
}

void DestroyWebRTCThread() {
    g_webrtc_thread.reset();
}

