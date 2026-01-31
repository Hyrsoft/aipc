/**
 * @file signaling.h
 * @brief WebRTC 信令客户端 - 基于 WebSocket 的 SDP/ICE 交换
 *
 * 提供与信令服务器通信的功能：
 * - WebSocket 连接管理
 * - SDP Offer/Answer 交换
 * - ICE 候选交换
 * - 房间管理
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#pragma once

#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <vector>

// 前向声明 libdatachannel 类型
namespace rtc {
class WebSocket;
}

// ============================================================================
// 信令状态枚举
// ============================================================================

enum class SignalingStatus {
    kDisconnected = 0,  ///< 未连接
    kConnecting,        ///< 连接中
    kConnected,         ///< 已连接
    kJoined,            ///< 已加入房间
    kPaired             ///< 已配对
};

// ============================================================================
// 信令错误枚举
// ============================================================================

enum class SignalingError {
    kNone = 0,
    kConnectionFailed,
    kSendFailed,
    kRoomFull,
    kRoomNotExists,
    kTimeout,
    kPeerOffline,
    kServerError
};

// ============================================================================
// 数据结构
// ============================================================================

/**
 * @brief 房间信息
 */
struct RoomInfo {
    std::string room_id;  ///< 房间 ID
    int num = 0;          ///< 房间人数
};

/**
 * @brief 连接请求参数
 */
struct ConnectionRequest {
    bool message = true;  ///< 是否开启消息通道
    bool audio = false;   ///< 是否开启音频通道
    bool video = true;    ///< 是否开启视频通道
};

// ============================================================================
// 回调类型定义
// ============================================================================

using SignalingStatusCallback = std::function<void(SignalingStatus)>;
using SignalingErrorCallback = std::function<void(SignalingError, const std::string&)>;
using OfferReceivedCallback = std::function<void(const std::string& sdp)>;
using AnswerReceivedCallback = std::function<void(const std::string& sdp)>;
using IceCandidateCallback = std::function<void(const std::string& candidate, const std::string& mid, int mline_index)>;
using ConnectionRequestCallback = std::function<void(const ConnectionRequest&)>;
using ConnectionResponseCallback = std::function<void(bool accepted)>;
using RoomInfoCallback = std::function<void(const RoomInfo&)>;
using WebRTCReadyCallback = std::function<void(const std::string& role, const std::string& peer_id)>;

// ============================================================================
// 信令配置
// ============================================================================

struct SignalingConfig {
    std::string device_id;               ///< 设备 ID（如：glasses_123456）
    std::string server_url;              ///< 服务器地址（如：ws://192.168.1.100:8000）
    bool auto_reconnect = true;          ///< 自动重连
    int reconnect_interval_ms = 5000;    ///< 重连间隔
    int max_reconnect_attempts = 0;      ///< 最大重连次数（0=无限）
};

// ============================================================================
// 信令客户端类
// ============================================================================

/**
 * @brief WebRTC 信令客户端
 * 
 * 负责与信令服务器通信，处理 SDP/ICE 交换
 * 设备端作为 offerer，发送 Offer，接收 Answer
 */
class SignalingClient {
public:
    explicit SignalingClient(const SignalingConfig& config);
    ~SignalingClient();

    // 禁用拷贝
    SignalingClient(const SignalingClient&) = delete;
    SignalingClient& operator=(const SignalingClient&) = delete;

    // ========================================================================
    // 连接管理
    // ========================================================================

    /**
     * @brief 连接到信令服务器
     * @return true 成功启动连接，false 失败
     */
    bool Connect();

    /**
     * @brief 断开连接
     */
    void Disconnect();

    /**
     * @brief 加入房间
     */
    bool JoinRoom();

    /**
     * @brief 离开房间
     */
    bool LeaveRoom();

    // ========================================================================
    // SDP/ICE 消息发送
    // ========================================================================

    /**
     * @brief 发送 SDP Offer
     * @param sdp SDP 字符串
     * @param target_device_id 目标设备 ID
     */
    bool SendOffer(const std::string& sdp, const std::string& target_device_id);

    /**
     * @brief 发送 SDP Answer
     * @param sdp SDP 字符串
     * @param target_device_id 目标设备 ID
     */
    bool SendAnswer(const std::string& sdp, const std::string& target_device_id);

    /**
     * @brief 发送 ICE 候选
     * @param candidate ICE 候选字符串
     * @param mid SDP mid
     * @param mline_index SDP mline index
     * @param target_device_id 目标设备 ID
     */
    bool SendIceCandidate(const std::string& candidate, const std::string& mid,
                          int mline_index, const std::string& target_device_id);

    /**
     * @brief 发送连接请求
     */
    bool SendConnectionRequest(const std::string& peer_id, const ConnectionRequest& request);

    // ========================================================================
    // 状态查询
    // ========================================================================

    SignalingStatus GetStatus() const { return status_.load(); }
    bool IsConnected() const { return status_.load() != SignalingStatus::kDisconnected; }
    bool IsPaired() const { return status_.load() == SignalingStatus::kPaired; }
    const std::string& GetDeviceId() const { return config_.device_id; }
    std::string GetPeerDeviceId() const;
    std::string GetRole() const;
    RoomInfo GetRoomInfo() const;

    // ========================================================================
    // 回调设置
    // ========================================================================

    void OnStatusChanged(SignalingStatusCallback callback);
    void OnError(SignalingErrorCallback callback);
    void OnOfferReceived(OfferReceivedCallback callback);
    void OnAnswerReceived(AnswerReceivedCallback callback);
    void OnIceCandidateReceived(IceCandidateCallback callback);
    void OnConnectionRequest(ConnectionRequestCallback callback);
    void OnConnectionResponse(ConnectionResponseCallback callback);
    void OnRoomInfoChanged(RoomInfoCallback callback);
    void OnWebRTCReady(WebRTCReadyCallback callback);

    // ========================================================================
    // 工具函数
    // ========================================================================

    static const char* StatusToString(SignalingStatus status);
    static const char* ErrorToString(SignalingError error);

private:
    // WebSocket 回调处理
    void HandleMessage(const std::string& message);
    void HandleOpen();
    void HandleClose();
    void HandleError(const std::string& error);

    // 消息处理
    void HandleRoleMessage(const std::string& role, const std::string& peer_id);
    void HandleOfferMessage(const std::string& from, const std::string& sdp);
    void HandleAnswerMessage(const std::string& from, const std::string& sdp);
    void HandleIceMessage(const std::string& from, const std::string& candidate, 
                         const std::string& mid, int mline_index);
    void HandleInfoMessage(const RoomInfo& info);
    void HandleErrorMessage(int code, const std::string& message);
    void HandleConnectionRequest(const ConnectionRequest& request);
    void HandleConnectionResponse(bool accepted);

    // 内部工具
    bool SendJsonMessage(const std::string& json_str);
    void SetStatus(SignalingStatus new_status);
    std::string ExtractRoomId(const std::string& device_id) const;
    uint64_t GetCurrentTimestamp() const;

    // 配置和状态
    SignalingConfig config_;
    std::atomic<SignalingStatus> status_{SignalingStatus::kDisconnected};
    std::shared_ptr<rtc::WebSocket> ws_;

    // 保护共享数据
    mutable std::mutex mutex_;
    std::string peer_device_id_;
    std::string role_;
    RoomInfo room_info_;

    // 回调函数
    mutable std::mutex callback_mutex_;
    SignalingStatusCallback status_callback_;
    SignalingErrorCallback error_callback_;
    OfferReceivedCallback offer_callback_;
    AnswerReceivedCallback answer_callback_;
    IceCandidateCallback ice_callback_;
    ConnectionRequestCallback conn_request_callback_;
    ConnectionResponseCallback conn_response_callback_;
    RoomInfoCallback room_info_callback_;
    WebRTCReadyCallback webrtc_ready_callback_;
};

