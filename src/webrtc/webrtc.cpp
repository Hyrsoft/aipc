/**
 * @file webrtc.cpp
 * @brief WebRTC 系统实现
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#include "webrtc.h"
#include "common/logger.h"

#include <rtc/rtc.hpp>
#include <rtc/frameinfo.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtcpreceivingsession.hpp>

#undef LOG_TAG
#define LOG_TAG "webrtc"

// ============================================================================
// WebRTCSystem 实现
// ============================================================================

WebRTCSystem::WebRTCSystem(const WebRTCConfig& config)
    : config_(config)
{
    LOG_INFO("WebRTC 系统创建");
}

WebRTCSystem::~WebRTCSystem() {
    Deinit();
    LOG_INFO("WebRTC 系统销毁");
}

WebRTCError WebRTCSystem::Init(std::shared_ptr<SignalingClient> signaling) {
    if (!signaling) {
        LOG_ERROR("信令模块为空");
        return WebRTCError::kSignalingFailed;
    }

    if (initialized_.load()) {
        LOG_WARN("已初始化");
        return WebRTCError::kNone;
    }

    if (!ValidateConfig()) {
        LOG_ERROR("配置验证失败");
        return WebRTCError::kUnknown;
    }

    LOG_INFO("开始初始化 WebRTC 系统...");
    signaling_ = std::move(signaling);

    // 设置信令回调
    signaling_->OnWebRTCReady([this](const std::string& role, const std::string& peer_id) {
        LOG_INFO("配对成功: role={}, peer={}", role, peer_id);
    });

    signaling_->OnOfferReceived([this](const std::string& sdp) {
        HandleRemoteOffer(sdp);
    });

    signaling_->OnAnswerReceived([this](const std::string& sdp) {
        HandleRemoteAnswer(sdp);
    });

    signaling_->OnIceCandidateReceived([this](const std::string& candidate, 
                                               const std::string& mid, 
                                               int mline_index) {
        HandleIceCandidate(candidate, mid, mline_index);
    });

    signaling_->OnConnectionRequest([this](const ConnectionRequest& request) {
        LOG_INFO("收到连接请求: video={}", request.video);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            current_request_ = request;
            peer_device_id_ = signaling_->GetPeerDeviceId();
        }
        // 自动接受连接并开始 WebRTC 流程
        StartWebRTCConnection();
    });

    signaling_->OnConnectionResponse([this](bool accepted) {
        if (accepted) {
            LOG_INFO("对方同意连接，开始 WebRTC 流程");
            StartWebRTCConnection();
        } else {
            LOG_WARN("对方拒绝连接");
            SetState(WebRTCState::kFailed);
            InvokeErrorCallback(WebRTCError::kConnectionFailed, "Connection rejected");
        }
    });

    initialized_.store(true);
    LOG_INFO("WebRTC 系统初始化完成");
    return WebRTCError::kNone;
}

void WebRTCSystem::Deinit() {
    if (!initialized_.load()) {
        return;
    }

    LOG_INFO("关闭 WebRTC 系统...");

    Disconnect();
    Cleanup();
    signaling_.reset();
    initialized_.store(false);
    SetState(WebRTCState::kIdle);

    LOG_INFO("WebRTC 系统已关闭");
}

bool WebRTCSystem::SendConnectionRequest(const std::string& peer_id, bool enable_video) {
    if (!IsInitialized() || !signaling_) {
        LOG_ERROR("系统未初始化");
        return false;
    }

    auto current = state_.load();
    if (current != WebRTCState::kIdle && current != WebRTCState::kDisconnected) {
        LOG_WARN("当前状态不允许发送连接请求");
        return false;
    }

    ConnectionRequest request;
    request.video = enable_video;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_request_ = request;
        peer_device_id_ = peer_id;
    }

    if (signaling_->SendConnectionRequest(peer_id, request)) {
        SetState(WebRTCState::kWaitingRequest);
        LOG_INFO("发送连接请求到: {}", peer_id);
        return true;
    }

    LOG_ERROR("发送连接请求失败");
    return false;
}

void WebRTCSystem::HandleRemoteOffer(const std::string& sdp) {
    if (!peer_connection_) {
        // 收到 Offer，需要创建 PeerConnection 并作为 answerer
        CreatePeerConnection();
        CreateVideoTrack(false);  // RecvOnly
    }

    ProcessRemoteOffer(sdp);
}

void WebRTCSystem::HandleRemoteAnswer(const std::string& sdp) {
    if (!peer_connection_) {
        LOG_ERROR("PeerConnection 未创建");
        return;
    }

    auto current = state_.load();
    if (current != WebRTCState::kSdpConnecting) {
        LOG_WARN("状态不正确，忽略 Answer");
        return;
    }

    try {
        LOG_INFO("处理远程 Answer");
        ProcessRemoteAnswer(sdp);
        SetState(WebRTCState::kSdpConnected);
    } catch (const std::exception& e) {
        LOG_ERROR("处理 Answer 失败: {}", e.what());
        SetState(WebRTCState::kFailed);
        InvokeErrorCallback(WebRTCError::kSdpNegotiationFailed, e.what());
    }
}

void WebRTCSystem::HandleIceCandidate(const std::string& candidate, 
                                       const std::string& mid, 
                                       int mline_index) {
    if (!peer_connection_) {
        // 缓存 ICE 候选
        std::lock_guard<std::mutex> lock(ice_mutex_);
        pending_ice_candidates_.emplace_back(candidate, mid, mline_index);
        LOG_DEBUG("缓存 ICE 候选，当前: {}", pending_ice_candidates_.size());
        return;
    }

    try {
        peer_connection_->addRemoteCandidate(rtc::Candidate(candidate, mid));
        LOG_DEBUG("添加远程 ICE 候选");
    } catch (const std::exception& e) {
        LOG_ERROR("添加 ICE 候选失败: {}", e.what());
    }
}

void WebRTCSystem::Disconnect() {
    auto current = state_.load();
    if (current == WebRTCState::kIdle || 
        current == WebRTCState::kDisconnected ||
        current == WebRTCState::kDisconnecting) {
        return;
    }

    LOG_INFO("断开 WebRTC 连接");
    SetState(WebRTCState::kDisconnecting);
    Cleanup();
    SetState(WebRTCState::kDisconnected);
}

void WebRTCSystem::SendVideoData(const uint8_t* data, size_t size, uint64_t timestamp) {
    if (!IsConnected() || !video_track_ || !video_track_->isOpen()) {
        return;
    }

    // 帧率控制（可选，编码器已经按帧率输出）
    // auto now = std::chrono::steady_clock::now();
    // if (last_video_send_time_ != std::chrono::steady_clock::time_point{}) {
    //     auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    //         now - last_video_send_time_);
    //     int interval_ms = 1000 / config_.video.fps;
    //     if (elapsed.count() < interval_ms) {
    //         return;
    //     }
    // }
    // last_video_send_time_ = now;

    try {
        // 将微秒时间戳转换为秒，供 RTP 时间戳计算使用
        // RTP 时间戳使用 90kHz 时钟，libdatachannel 会自动计算
        auto sample_time = std::chrono::duration<double>(timestamp / 1000000.0);
        rtc::FrameInfo frame_info(sample_time);
        
        video_track_->sendFrame(
            reinterpret_cast<const std::byte*>(data), size, frame_info);

        // 更新统计
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.video_packets_sent++;
            stats_.video_bytes_sent += size;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("发送视频失败: {}", e.what());
    }
}

bool WebRTCSystem::SendDataMessage(const std::string& message) {
    if (!data_channel_ || !data_channel_->isOpen()) {
        LOG_WARN("DataChannel 未打开");
        return false;
    }

    try {
        data_channel_->send(message);
        LOG_DEBUG("发送消息: {}", message);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("发送消息失败: {}", e.what());
        return false;
    }
}

bool WebRTCSystem::IsConnected() const {
    auto s = state_.load();
    return s == WebRTCState::kIceConnected || s == WebRTCState::kConnected;
}

bool WebRTCSystem::IsConnecting() const {
    auto s = state_.load();
    return s == WebRTCState::kWaitingRequest ||
           s == WebRTCState::kSdpConnecting ||
           s == WebRTCState::kSdpConnected ||
           s == WebRTCState::kIceConnecting;
}

std::string WebRTCSystem::GetPeerId() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return peer_device_id_;
}

WebRTCStats WebRTCSystem::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    WebRTCStats result = stats_;
    
    if (connection_start_time_ != std::chrono::steady_clock::time_point{} && IsConnected()) {
        auto now = std::chrono::steady_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - connection_start_time_);
        result.connection_duration_ms = diff.count();
    }
    
    return result;
}

void WebRTCSystem::ResetStats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = WebRTCStats{};
}

void WebRTCSystem::OnStateChanged(StateCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    state_callback_ = std::move(callback);
}

void WebRTCSystem::OnError(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    error_callback_ = std::move(callback);
}

void WebRTCSystem::OnDataMessage(MessageCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    message_callback_ = std::move(callback);
}

const char* WebRTCSystem::StateToString(WebRTCState state) {
    switch (state) {
        case WebRTCState::kIdle: return "Idle";
        case WebRTCState::kWaitingRequest: return "WaitingRequest";
        case WebRTCState::kSdpConnecting: return "SdpConnecting";
        case WebRTCState::kSdpConnected: return "SdpConnected";
        case WebRTCState::kIceConnecting: return "IceConnecting";
        case WebRTCState::kIceConnected: return "IceConnected";
        case WebRTCState::kConnected: return "Connected";
        case WebRTCState::kDisconnecting: return "Disconnecting";
        case WebRTCState::kDisconnected: return "Disconnected";
        case WebRTCState::kFailed: return "Failed";
        default: return "Unknown";
    }
}

// ============================================================================
// 私有方法实现
// ============================================================================

void WebRTCSystem::SetState(WebRTCState new_state) {
    auto old_state = state_.exchange(new_state);
    if (old_state != new_state) {
        LOG_INFO("状态: {} -> {}", StateToString(old_state), StateToString(new_state));
        HandleStateTransition(old_state, new_state);
    }
}

void WebRTCSystem::HandleStateTransition(WebRTCState old_state, WebRTCState new_state) {
    // 通知外部
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (state_callback_) {
            state_callback_(new_state);
        }
    }

    switch (new_state) {
        case WebRTCState::kConnected:
            connection_start_time_ = std::chrono::steady_clock::now();
            LOG_INFO("WebRTC 连接已建立");
            break;
        case WebRTCState::kDisconnected:
        case WebRTCState::kFailed:
            connection_start_time_ = std::chrono::steady_clock::time_point{};
            break;
        default:
            break;
    }
}

void WebRTCSystem::StartWebRTCConnection() {
    try {
        LOG_INFO("开始建立 WebRTC 连接...");

        CreatePeerConnection();
        CreateVideoTrack(true);  // SendOnly
        SetupDataChannel();

        SetState(WebRTCState::kSdpConnecting);
        GenerateAndSendOffer();
    } catch (const std::exception& e) {
        LOG_ERROR("建立连接失败: {}", e.what());
        SetState(WebRTCState::kFailed);
        InvokeErrorCallback(WebRTCError::kConnectionFailed, e.what());
    }
}

void WebRTCSystem::CreatePeerConnection() {
    rtc::Configuration config;

    // 添加 ICE 服务器
    for (const auto& stun : config_.ice.stun_servers) {
        config.iceServers.emplace_back(stun);
        LOG_INFO("添加 STUN 服务器: {}", stun);
    }
    for (const auto& turn : config_.ice.turn_servers) {
        config.iceServers.emplace_back(turn);
        LOG_INFO("添加 TURN 服务器: {}", turn);
    }

    config.iceTransportPolicy = config_.ice.use_relay_only 
        ? rtc::TransportPolicy::Relay 
        : rtc::TransportPolicy::All;
    config.disableAutoNegotiation = true;

    peer_connection_ = std::make_shared<rtc::PeerConnection>(config);
    SetupPeerConnectionCallbacks();

    sdp_exchange_completed_.store(false);
    {
        std::lock_guard<std::mutex> lock(ice_mutex_);
        pending_ice_candidates_.clear();
    }

    LOG_INFO("PeerConnection 创建成功");
}

void WebRTCSystem::CreateVideoTrack(bool send_only) {
    if (!peer_connection_) {
        LOG_ERROR("PeerConnection 未创建");
        return;
    }

    try {
        auto direction = send_only 
            ? rtc::Description::Direction::SendOnly 
            : rtc::Description::Direction::RecvOnly;

        LOG_INFO("创建视频轨道: {}x{} @ {}fps, direction={}", 
                 config_.video.width, config_.video.height, config_.video.fps,
                 send_only ? "SendOnly" : "RecvOnly");

        rtc::Description::Video video_desc("video", direction);
        video_desc.addH264Codec(config_.video.payload_type);
        video_desc.addSSRC(config_.video.ssrc, "video", "stream1", "video");

        video_track_ = peer_connection_->addTrack(video_desc);

        if (send_only) {
            // 配置 RTP 发送组件
            video_rtp_config_ = std::make_shared<rtc::RtpPacketizationConfig>(
                config_.video.ssrc, "video", config_.video.payload_type,
                rtc::H264RtpPacketizer::ClockRate);

            video_packetizer_ = std::make_shared<rtc::H264RtpPacketizer>(
                rtc::NalUnit::Separator::StartSequence, video_rtp_config_, 1200);

            video_sr_reporter_ = std::make_shared<rtc::RtcpSrReporter>(video_rtp_config_);
            video_packetizer_->addToChain(video_sr_reporter_);

            video_rtcp_session_ = std::make_shared<rtc::RtcpReceivingSession>();
            video_packetizer_->addToChain(video_rtcp_session_);

            video_track_->setMediaHandler(video_packetizer_);

            video_track_->onOpen([this]() {
                OnTrackOpen();
            });
        }

        LOG_INFO("视频轨道创建成功");
    } catch (const std::exception& e) {
        LOG_ERROR("创建视频轨道失败: {}", e.what());
    }
}

void WebRTCSystem::SetupDataChannel() {
    if (!peer_connection_) {
        LOG_ERROR("PeerConnection 未创建");
        return;
    }

    try {
        data_channel_ = peer_connection_->createDataChannel("message");

        data_channel_->onOpen([this]() {
            OnDataChannelOpen();
        });

        data_channel_->onClosed([this]() {
            LOG_INFO("DataChannel 已关闭");
        });

        data_channel_->onMessage([this](auto data) {
            if (std::holds_alternative<std::string>(data)) {
                OnDataChannelMessage(std::get<std::string>(data));
            }
        });

        LOG_INFO("DataChannel 创建成功");
    } catch (const std::exception& e) {
        LOG_ERROR("创建 DataChannel 失败: {}", e.what());
    }
}

void WebRTCSystem::GenerateAndSendOffer() {
    if (!peer_connection_) {
        LOG_ERROR("PeerConnection 未创建");
        return;
    }

    try {
        LOG_INFO("生成 Offer");
        peer_connection_->setLocalDescription();
    } catch (const std::exception& e) {
        LOG_ERROR("生成 Offer 失败: {}", e.what());
        SetState(WebRTCState::kFailed);
        InvokeErrorCallback(WebRTCError::kSdpNegotiationFailed, e.what());
    }
}

void WebRTCSystem::ProcessRemoteOffer(const std::string& sdp) {
    if (!peer_connection_) {
        LOG_ERROR("PeerConnection 未创建");
        return;
    }

    try {
        rtc::Description remote_desc(sdp, rtc::Description::Type::Offer);
        peer_connection_->setRemoteDescription(remote_desc);
        
        // 作为 answerer，生成 Answer
        peer_connection_->setLocalDescription();
        SetState(WebRTCState::kSdpConnecting);
    } catch (const std::exception& e) {
        LOG_ERROR("处理 Offer 失败: {}", e.what());
        SetState(WebRTCState::kFailed);
    }
}

void WebRTCSystem::ProcessRemoteAnswer(const std::string& sdp) {
    if (!peer_connection_) {
        LOG_ERROR("PeerConnection 未创建");
        return;
    }

    rtc::Description remote_desc(sdp, rtc::Description::Type::Answer);
    peer_connection_->setRemoteDescription(remote_desc);

    sdp_exchange_completed_.store(true);

    LOG_INFO("SDP 交换完成，发送缓存的 ICE 候选");
    FlushPendingIceCandidates();
}

void WebRTCSystem::FlushPendingIceCandidates() {
    std::lock_guard<std::mutex> lock(ice_mutex_);
    
    if (pending_ice_candidates_.empty()) {
        return;
    }

    LOG_INFO("发送缓存的 ICE 候选: {}", pending_ice_candidates_.size());

    std::string peer_id;
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        peer_id = peer_device_id_;
    }

    for (const auto& [candidate, mid, mline_index] : pending_ice_candidates_) {
        if (signaling_ && signaling_->IsPaired()) {
            signaling_->SendIceCandidate(candidate, mid, mline_index, peer_id);
        }
    }

    pending_ice_candidates_.clear();
}

void WebRTCSystem::SetupPeerConnectionCallbacks() {
    if (!peer_connection_) return;

    peer_connection_->onLocalDescription([this](rtc::Description desc) {
        OnLocalDescription(std::string(desc), 
                          desc.type() == rtc::Description::Type::Offer ? "offer" : "answer");
    });

    peer_connection_->onLocalCandidate([this](rtc::Candidate candidate) {
        OnLocalCandidate(std::string(candidate), candidate.mid());
    });

    peer_connection_->onStateChange([this](rtc::PeerConnection::State state) {
        OnPeerConnectionStateChange(static_cast<int>(state));
    });

    peer_connection_->onIceStateChange([this](rtc::PeerConnection::IceState state) {
        OnIceStateChange(static_cast<int>(state));
    });

    peer_connection_->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
        LOG_INFO("收到远程 DataChannel: {}", dc->label());
        data_channel_ = dc;
        
        data_channel_->onOpen([this]() {
            OnDataChannelOpen();
        });
        
        data_channel_->onMessage([this](auto data) {
            if (std::holds_alternative<std::string>(data)) {
                OnDataChannelMessage(std::get<std::string>(data));
            }
        });
    });
}

void WebRTCSystem::OnLocalDescription(const std::string& sdp, const std::string& type) {
    if (!signaling_ || !signaling_->IsPaired()) {
        LOG_WARN("信令未准备好");
        return;
    }

    std::string peer_id;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        peer_id = peer_device_id_;
    }

    if (type == "offer") {
        LOG_INFO("发送 Offer SDP");
        signaling_->SendOffer(sdp, peer_id);
    } else {
        LOG_INFO("发送 Answer SDP");
        signaling_->SendAnswer(sdp, peer_id);
        sdp_exchange_completed_.store(true);
        SetState(WebRTCState::kSdpConnected);
    }
}

void WebRTCSystem::OnLocalCandidate(const std::string& candidate, const std::string& mid) {
    if (!signaling_ || !signaling_->IsPaired()) {
        return;
    }

    if (sdp_exchange_completed_.load()) {
        std::string peer_id;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            peer_id = peer_device_id_;
        }
        signaling_->SendIceCandidate(candidate, mid, 0, peer_id);
        LOG_DEBUG("发送 ICE 候选");
    } else {
        std::lock_guard<std::mutex> lock(ice_mutex_);
        pending_ice_candidates_.emplace_back(candidate, mid, 0);
        LOG_DEBUG("缓存 ICE 候选: {}", pending_ice_candidates_.size());
    }
}

void WebRTCSystem::OnPeerConnectionStateChange(int state) {
    auto pc_state = static_cast<rtc::PeerConnection::State>(state);
    LOG_INFO("PeerConnection 状态: {}", static_cast<int>(pc_state));

    switch (pc_state) {
        case rtc::PeerConnection::State::Connected: {
            auto current = state_.load();
            if (current == WebRTCState::kIceConnected || current == WebRTCState::kIceConnecting) {
                SetState(WebRTCState::kConnected);
            }
            break;
        }
        case rtc::PeerConnection::State::Failed:
            SetState(WebRTCState::kFailed);
            InvokeErrorCallback(WebRTCError::kConnectionFailed, "PeerConnection failed");
            break;
        case rtc::PeerConnection::State::Closed:
            if (state_.load() != WebRTCState::kDisconnecting) {
                SetState(WebRTCState::kDisconnected);
            }
            break;
        default:
            break;
    }
}

void WebRTCSystem::OnIceStateChange(int state) {
    auto ice_state = static_cast<rtc::PeerConnection::IceState>(state);
    LOG_INFO("ICE 状态: {}", static_cast<int>(ice_state));

    auto current = state_.load();

    switch (ice_state) {
        case rtc::PeerConnection::IceState::Checking:
            if (current == WebRTCState::kSdpConnected) {
                SetState(WebRTCState::kIceConnecting);
            }
            break;
        case rtc::PeerConnection::IceState::Connected:
        case rtc::PeerConnection::IceState::Completed:
            if (current == WebRTCState::kIceConnecting || current == WebRTCState::kSdpConnected) {
                SetState(WebRTCState::kIceConnected);
            }
            break;
        case rtc::PeerConnection::IceState::Failed:
            SetState(WebRTCState::kFailed);
            InvokeErrorCallback(WebRTCError::kIceFailed, "ICE connection failed");
            break;
        default:
            break;
    }
}

void WebRTCSystem::OnTrackOpen() {
    LOG_INFO("视频轨道已打开，可以发送数据");
}

void WebRTCSystem::OnDataChannelOpen() {
    LOG_INFO("DataChannel 已打开");
}

void WebRTCSystem::OnDataChannelMessage(const std::string& message) {
    LOG_INFO("收到消息: {}", message);
    
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (message_callback_) {
        message_callback_(message);
    }
}

void WebRTCSystem::Cleanup() {
    LOG_INFO("清理 WebRTC 资源...");

    // 关闭轨道
    if (video_track_) {
        try {
            video_track_->close();
        } catch (...) {}
        video_track_.reset();
    }

    // 清理 RTP 组件
    video_packetizer_.reset();
    video_rtp_config_.reset();
    video_sr_reporter_.reset();
    video_rtcp_session_.reset();

    // 关闭 DataChannel
    if (data_channel_) {
        try {
            data_channel_->close();
        } catch (...) {}
        data_channel_.reset();
    }

    // 关闭 PeerConnection
    if (peer_connection_) {
        try {
            peer_connection_->close();
        } catch (...) {}
        peer_connection_.reset();
    }

    // 清理回调
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        state_callback_ = nullptr;
        error_callback_ = nullptr;
        message_callback_ = nullptr;
    }

    // 重置统计
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_ = WebRTCStats{};
    }

    // 清理 ICE 缓存
    {
        std::lock_guard<std::mutex> lock(ice_mutex_);
        pending_ice_candidates_.clear();
    }
    sdp_exchange_completed_.store(false);

    LOG_INFO("资源清理完成");
}

bool WebRTCSystem::ValidateConfig() const {
    if (config_.video.codec != "h264") {
        LOG_ERROR("仅支持 H.264 编码");
        return false;
    }

    if (config_.video.payload_type < 96 || config_.video.payload_type > 127) {
        LOG_ERROR("Payload Type 必须在 96-127 范围内");
        return false;
    }

    LOG_INFO("配置验证通过");
    return true;
}

void WebRTCSystem::InvokeErrorCallback(WebRTCError error, const std::string& message) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (error_callback_) {
        try {
            error_callback_(error, message);
        } catch (const std::exception& e) {
            LOG_ERROR("错误回调异常: {}", e.what());
        }
    }
}

// ============================================================================
// HTTP 信令模式实现
// ============================================================================

std::string WebRTCSystem::CreateOfferForHttp() {
    LOG_INFO("[HTTP] 创建 Offer...");

    // 清理之前的连接
    if (peer_connection_) {
        Cleanup();
    }

    http_mode_.store(true);
    pending_local_sdp_.clear();
    {
        std::lock_guard<std::mutex> lock(local_ice_mutex_);
        local_ice_candidates_.clear();
    }

    try {
        CreatePeerConnection();
        CreateVideoTrack(true);  // SendOnly
        // SetupDataChannel();  // 暂时不需要 DataChannel

        SetState(WebRTCState::kSdpConnecting);

        // 设置回调来捕获本地描述
        std::mutex sdp_mutex;
        std::condition_variable sdp_cv;
        bool sdp_ready = false;
        std::string local_sdp;

        peer_connection_->onLocalDescription([&](rtc::Description desc) {
            std::lock_guard<std::mutex> lock(sdp_mutex);
            local_sdp = std::string(desc);
            sdp_ready = true;
            sdp_cv.notify_one();
            LOG_INFO("[HTTP] 本地描述已生成");
        });

        // 重新设置 ICE 候选回调以收集本地候选
        peer_connection_->onLocalCandidate([this](rtc::Candidate candidate) {
            std::lock_guard<std::mutex> lock(local_ice_mutex_);
            local_ice_candidates_.emplace_back(std::string(candidate), candidate.mid());
            LOG_DEBUG("[HTTP] 收集到本地 ICE 候选: {}", candidate.mid());
        });

        // 生成 Offer
        peer_connection_->setLocalDescription();

        // 等待 SDP 生成
        std::unique_lock<std::mutex> lock(sdp_mutex);
        if (!sdp_cv.wait_for(lock, std::chrono::seconds(5), [&]{ return sdp_ready; })) {
            LOG_ERROR("[HTTP] 等待 SDP 超时");
            return "";
        }

        pending_local_sdp_ = local_sdp;
        LOG_INFO("[HTTP] Offer 创建成功, 长度: {}", local_sdp.length());
        return local_sdp;

    } catch (const std::exception& e) {
        LOG_ERROR("[HTTP] 创建 Offer 失败: {}", e.what());
        SetState(WebRTCState::kFailed);
        return "";
    }
}

bool WebRTCSystem::SetAnswerFromHttp(const std::string& sdp) {
    LOG_INFO("[HTTP] 设置 Answer...");

    if (!peer_connection_) {
        LOG_ERROR("[HTTP] PeerConnection 不存在");
        return false;
    }

    try {
        rtc::Description remote_desc(sdp, rtc::Description::Type::Answer);
        peer_connection_->setRemoteDescription(remote_desc);

        sdp_exchange_completed_.store(true);
        SetState(WebRTCState::kSdpConnected);

        LOG_INFO("[HTTP] Answer 设置成功");
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR("[HTTP] 设置 Answer 失败: {}", e.what());
        SetState(WebRTCState::kFailed);
        return false;
    }
}

bool WebRTCSystem::AddIceCandidateFromHttp(const std::string& candidate, const std::string& mid) {
    LOG_DEBUG("[HTTP] 添加远程 ICE 候选: mid={}", mid);

    if (!peer_connection_) {
        LOG_ERROR("[HTTP] PeerConnection 不存在");
        return false;
    }

    try {
        rtc::Candidate rtc_candidate(candidate, mid);
        peer_connection_->addRemoteCandidate(rtc_candidate);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("[HTTP] 添加 ICE 候选失败: {}", e.what());
        return false;
    }
}

std::vector<std::pair<std::string, std::string>> WebRTCSystem::GetLocalIceCandidates() {
    std::lock_guard<std::mutex> lock(local_ice_mutex_);
    return local_ice_candidates_;
}

bool WebRTCSystem::HasPendingLocalIceCandidates() const {
    std::lock_guard<std::mutex> lock(local_ice_mutex_);
    return !local_ice_candidates_.empty();
}

