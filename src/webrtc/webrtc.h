/**
 * @file webrtc.h
 * @brief WebRTC 系统封装 - 基于 libdatachannel 的 WebRTC 连接管理
 *
 * 提供 WebRTC 连接的完整生命周期管理：
 * - PeerConnection 创建和管理
 * - 音视频轨道配置
 * - SDP 协商
 * - ICE 连接
 * - 媒体数据发送
 *
 * 与 RTSP 和 File 模块并列，作为视频流的第三条推送路径
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#pragma once

#include <memory>
#include <functional>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>

#include "signaling.h"

// 前向声明 libdatachannel 类型
namespace rtc {
class PeerConnection;
class Track;
class DataChannel;
class RtpPacketizationConfig;
class H264RtpPacketizer;
class RtcpSrReporter;
class RtcpReceivingSession;
class Description;
class Candidate;
}

// ============================================================================
// WebRTC 状态枚举
// ============================================================================

enum class WebRTCState {
    kIdle = 0,              ///< 空闲状态
    kWaitingRequest,        ///< 等待连接请求
    kSdpConnecting,         ///< SDP 协商中
    kSdpConnected,          ///< SDP 协商完成
    kIceConnecting,         ///< ICE 连接中
    kIceConnected,          ///< ICE 连接完成
    kConnected,             ///< WebRTC 连接建立
    kDisconnecting,         ///< 断开连接中
    kDisconnected,          ///< 已断开
    kFailed                 ///< 连接失败
};

// ============================================================================
// WebRTC 错误枚举
// ============================================================================

enum class WebRTCError {
    kNone = 0,
    kSignalingFailed,
    kSdpNegotiationFailed,
    kIceFailed,
    kConnectionFailed,
    kTimeout,
    kUnknown
};

// ============================================================================
// 回调类型定义
// ============================================================================

using StateCallback = std::function<void(WebRTCState)>;
using ErrorCallback = std::function<void(WebRTCError, const std::string&)>;
using DataCallback = std::function<void(const uint8_t*, size_t)>;
using MessageCallback = std::function<void(const std::string&)>;

// ============================================================================
// WebRTC 配置
// ============================================================================

struct WebRTCConfig {
    // 视频配置
    struct {
        std::string codec = "h264";
        int width = 1920;
        int height = 1080;
        int fps = 30;
        int payload_type = 96;
        uint32_t ssrc = 1;
    } video;

    // ICE 配置
    struct {
        std::vector<std::string> stun_servers = {"stun:stun.l.google.com:19302"};
        std::vector<std::string> turn_servers;
        bool use_relay_only = false;
        int timeout_ms = 15000;
    } ice;
};

// ============================================================================
// 统计信息
// ============================================================================

struct WebRTCStats {
    uint64_t video_packets_sent = 0;
    uint64_t video_bytes_sent = 0;
    uint64_t connection_duration_ms = 0;
};

// ============================================================================
// WebRTC 系统类
// ============================================================================

/**
 * @brief WebRTC 系统
 * 
 * 管理 WebRTC 连接的完整生命周期
 * 支持发送 H.264 编码的视频流
 */
class WebRTCSystem {
public:
    explicit WebRTCSystem(const WebRTCConfig& config = WebRTCConfig{});
    ~WebRTCSystem();

    // 禁用拷贝
    WebRTCSystem(const WebRTCSystem&) = delete;
    WebRTCSystem& operator=(const WebRTCSystem&) = delete;

    // ========================================================================
    // 生命周期管理
    // ========================================================================

    /**
     * @brief 初始化 WebRTC 系统
     * @param signaling 信令客户端
     * @return 错误码
     */
    WebRTCError Init(std::shared_ptr<SignalingClient> signaling);

    /**
     * @brief 关闭 WebRTC 系统
     */
    void Deinit();

    /**
     * @brief 检查是否已初始化
     */
    bool IsInitialized() const { return initialized_.load(); }

    // ========================================================================
    // 连接管理
    // ========================================================================

    /**
     * @brief 发送连接请求（作为 offerer）
     */
    bool SendConnectionRequest(const std::string& peer_id, bool enable_video = true);

    /**
     * @brief 处理远程 Offer（作为 answerer）
     */
    void HandleRemoteOffer(const std::string& sdp);

    /**
     * @brief 处理远程 Answer
     */
    void HandleRemoteAnswer(const std::string& sdp);

    /**
     * @brief 处理远程 ICE 候选
     */
    void HandleIceCandidate(const std::string& candidate, const std::string& mid, int mline_index);

    /**
     * @brief 断开连接
     */
    void Disconnect();

    // ========================================================================
    // 媒体发送
    // ========================================================================

    /**
     * @brief 发送视频数据
     * @param data H.264 编码数据
     * @param size 数据大小
     * @param timestamp 时间戳（微秒）
     */
    void SendVideoData(const uint8_t* data, size_t size, uint64_t timestamp);

    /**
     * @brief 发送 DataChannel 消息
     */
    bool SendDataMessage(const std::string& message);

    // ========================================================================
    // HTTP 信令模式 API（用于网页直连）
    // ========================================================================

    /**
     * @brief 创建 Offer 用于 HTTP 信令
     * @return SDP Offer 字符串，失败返回空
     */
    std::string CreateOfferForHttp();

    /**
     * @brief 处理来自 HTTP 的 Answer
     * @param sdp Answer SDP
     * @return true 成功
     */
    bool SetAnswerFromHttp(const std::string& sdp);

    /**
     * @brief 添加来自 HTTP 的 ICE 候选
     */
    bool AddIceCandidateFromHttp(const std::string& candidate, const std::string& mid);

    /**
     * @brief 获取本地 ICE 候选列表（用于 HTTP 响应）
     */
    std::vector<std::pair<std::string, std::string>> GetLocalIceCandidates();

    /**
     * @brief 检查是否有待发送的本地 ICE 候选
     */
    bool HasPendingLocalIceCandidates() const;

    // ========================================================================
    // 状态查询
    // ========================================================================

    WebRTCState GetState() const { return state_.load(); }
    bool IsConnected() const;
    bool IsConnecting() const;
    std::string GetPeerId() const;
    WebRTCStats GetStats() const;
    void ResetStats();

    // ========================================================================
    // 回调设置
    // ========================================================================

    void OnStateChanged(StateCallback callback);
    void OnError(ErrorCallback callback);
    void OnDataMessage(MessageCallback callback);

    // ========================================================================
    // 工具函数
    // ========================================================================

    static const char* StateToString(WebRTCState state);

private:
    // 内部方法
    void SetState(WebRTCState new_state);
    void HandleStateTransition(WebRTCState old_state, WebRTCState new_state);
    
    // WebRTC 连接流程
    void StartWebRTCConnection();
    void CreatePeerConnection();
    void CreateVideoTrack(bool send_only);
    void SetupDataChannel();
    void GenerateAndSendOffer();
    void ProcessRemoteOffer(const std::string& sdp);
    void ProcessRemoteAnswer(const std::string& sdp);
    void FlushPendingIceCandidates();

    // PeerConnection 回调
    void SetupPeerConnectionCallbacks();
    void OnLocalDescription(const std::string& sdp, const std::string& type);
    void OnLocalCandidate(const std::string& candidate, const std::string& mid);
    void OnPeerConnectionStateChange(int state);
    void OnIceStateChange(int state);
    void OnTrackOpen();
    void OnDataChannelOpen();
    void OnDataChannelMessage(const std::string& message);

    // 工具方法
    void Cleanup();
    bool ValidateConfig() const;
    void InvokeErrorCallback(WebRTCError error, const std::string& message);

    // 配置和状态
    WebRTCConfig config_;
    std::atomic<bool> initialized_{false};
    std::atomic<WebRTCState> state_{WebRTCState::kIdle};

    // 保护共享数据
    mutable std::mutex state_mutex_;
    std::string peer_device_id_;
    ConnectionRequest current_request_;

    // WebRTC 核心组件
    std::shared_ptr<SignalingClient> signaling_;
    std::shared_ptr<rtc::PeerConnection> peer_connection_;
    std::shared_ptr<rtc::Track> video_track_;
    std::shared_ptr<rtc::DataChannel> data_channel_;

    // RTP 组件
    std::shared_ptr<rtc::RtpPacketizationConfig> video_rtp_config_;
    std::shared_ptr<rtc::H264RtpPacketizer> video_packetizer_;
    std::shared_ptr<rtc::RtcpSrReporter> video_sr_reporter_;
    std::shared_ptr<rtc::RtcpReceivingSession> video_rtcp_session_;

    // 发送控制
    std::chrono::steady_clock::time_point last_video_send_time_;
    uint64_t first_video_timestamp_{0};  // 第一帧的时间戳，用于计算相对时间
    bool keyframe_received_{false};      // 是否已收到关键帧

    // 回调函数
    mutable std::mutex callback_mutex_;
    StateCallback state_callback_;
    ErrorCallback error_callback_;
    MessageCallback message_callback_;

    // 统计信息
    mutable std::mutex stats_mutex_;
    WebRTCStats stats_;
    std::chrono::steady_clock::time_point connection_start_time_;

    // ICE 候选缓存
    std::mutex ice_mutex_;
    std::vector<std::tuple<std::string, std::string, int>> pending_ice_candidates_;
    std::atomic<bool> sdp_exchange_completed_{false};

    // HTTP 信令模式的本地 ICE 候选缓存
    mutable std::mutex local_ice_mutex_;
    std::vector<std::pair<std::string, std::string>> local_ice_candidates_;
    std::string pending_local_sdp_;
    std::atomic<bool> http_mode_{false};
};

