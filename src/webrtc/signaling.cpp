/**
 * @file signaling.cpp
 * @brief WebRTC 信令客户端实现
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#include "signaling.h"
#include "common/logger.h"

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <sstream>

#undef LOG_TAG
#define LOG_TAG "signaling"

using json = nlohmann::json;

// ============================================================================
// 错误码常量
// ============================================================================

namespace {
constexpr int ERROR_CODE_ROOM_FULL = 1001;
constexpr int ERROR_CODE_ROOM_NOT_EXISTS = 1002;
constexpr int ERROR_CODE_MESSAGE_FORMAT_ERROR = 1003;
constexpr int ERROR_CODE_DEVICE_ID_ERROR = 1004;
constexpr int ERROR_CODE_CONNECTION_TIMEOUT = 1005;
constexpr int ERROR_CODE_PEER_OFFLINE = 1006;
constexpr int ERROR_CODE_SERVER_ERROR = 1007;
}

// ============================================================================
// SignalingClient 实现
// ============================================================================

SignalingClient::SignalingClient(const SignalingConfig& config)
    : config_(config)
{
    room_info_.room_id = ExtractRoomId(config_.device_id);
    room_info_.num = 0;
    LOG_INFO("信令客户端创建: deviceId={}, serverUrl={}", 
             config_.device_id, config_.server_url);
}

SignalingClient::~SignalingClient() {
    Disconnect();
}

bool SignalingClient::Connect() {
    if (status_.load() != SignalingStatus::kDisconnected) {
        LOG_WARN("已在连接状态");
        return false;
    }

    try {
        SetStatus(SignalingStatus::kConnecting);
        
        ws_ = std::make_shared<rtc::WebSocket>();
        
        // 设置 WebSocket 回调
        ws_->onOpen([this]() {
            HandleOpen();
        });
        
        ws_->onClosed([this]() {
            HandleClose();
        });
        
        ws_->onError([this](std::string error) {
            HandleError(error);
        });
        
        ws_->onMessage([this](auto message) {
            if (std::holds_alternative<std::string>(message)) {
                HandleMessage(std::get<std::string>(message));
            }
        });
        
        // 连接到信令服务器
        ws_->open(config_.server_url);
        LOG_INFO("正在连接信令服务器: {}", config_.server_url);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("连接异常: {}", e.what());
        SetStatus(SignalingStatus::kDisconnected);
        return false;
    }
}

void SignalingClient::Disconnect() {
    auto current = status_.load();
    if (current == SignalingStatus::kDisconnected) {
        return;
    }

    // 如果已加入房间，先离开
    if (current == SignalingStatus::kJoined || current == SignalingStatus::kPaired) {
        LeaveRoom();
    }

    // 断开 WebSocket
    if (ws_) {
        try {
            ws_->close();
        } catch (...) {}
        ws_.reset();
    }

    SetStatus(SignalingStatus::kDisconnected);

    // 清理状态
    {
        std::lock_guard<std::mutex> lock(mutex_);
        peer_device_id_.clear();
        role_.clear();
        room_info_.num = 0;
    }

    LOG_INFO("已断开信令连接");
}

bool SignalingClient::JoinRoom() {
    if (status_.load() != SignalingStatus::kConnected) {
        LOG_WARN("未连接到服务器，无法加入房间");
        return false;
    }

    json msg = {
        {"type", "join"},
        {"from", config_.device_id},
        {"to", "server"},
        {"time", GetCurrentTimestamp()}
    };

    if (SendJsonMessage(msg.dump())) {
        LOG_INFO("发送加入房间请求");
        SetStatus(SignalingStatus::kJoined);
        return true;
    }
    return false;
}

bool SignalingClient::LeaveRoom() {
    if (status_.load() == SignalingStatus::kDisconnected) {
        return false;
    }

    json msg = {
        {"type", "leave"},
        {"from", config_.device_id},
        {"to", "server"},
        {"time", GetCurrentTimestamp()}
    };

    if (SendJsonMessage(msg.dump())) {
        LOG_INFO("发送离开房间请求");
        SetStatus(SignalingStatus::kConnected);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            peer_device_id_.clear();
            role_.clear();
        }
        return true;
    }
    return false;
}

bool SignalingClient::SendOffer(const std::string& sdp, const std::string& target_device_id) {
    auto current = status_.load();
    if (current != SignalingStatus::kJoined && current != SignalingStatus::kPaired) {
        LOG_WARN("状态不正确，无法发送 Offer");
        return false;
    }

    json msg = {
        {"type", "offer"},
        {"from", config_.device_id},
        {"to", target_device_id},
        {"data", {{"sdp", sdp}}},
        {"time", GetCurrentTimestamp()}
    };

    LOG_INFO("发送 SDP Offer 到 {}", target_device_id);
    return SendJsonMessage(msg.dump());
}

bool SignalingClient::SendAnswer(const std::string& sdp, const std::string& target_device_id) {
    auto current = status_.load();
    if (current != SignalingStatus::kJoined && current != SignalingStatus::kPaired) {
        LOG_WARN("状态不正确，无法发送 Answer");
        return false;
    }

    json msg = {
        {"type", "answer"},
        {"from", config_.device_id},
        {"to", target_device_id},
        {"data", {{"sdp", sdp}}},
        {"time", GetCurrentTimestamp()}
    };

    LOG_INFO("发送 SDP Answer 到 {}", target_device_id);
    return SendJsonMessage(msg.dump());
}

bool SignalingClient::SendIceCandidate(const std::string& candidate, const std::string& mid,
                                        int mline_index, const std::string& target_device_id) {
    auto current = status_.load();
    if (current != SignalingStatus::kJoined && current != SignalingStatus::kPaired) {
        return false;
    }

    json msg = {
        {"type", "ice"},
        {"from", config_.device_id},
        {"to", target_device_id},
        {"data", {
            {"candidate", candidate},
            {"sdpMid", mid},
            {"sdpMLineIndex", mline_index}
        }},
        {"time", GetCurrentTimestamp()}
    };

    LOG_DEBUG("发送 ICE 候选");
    return SendJsonMessage(msg.dump());
}

bool SignalingClient::SendConnectionRequest(const std::string& peer_id, const ConnectionRequest& request) {
    auto current = status_.load();
    if (current != SignalingStatus::kJoined && current != SignalingStatus::kPaired) {
        LOG_WARN("状态不正确，无法发送连接请求");
        return false;
    }

    json msg = {
        {"type", "get_connect"},
        {"from", config_.device_id},
        {"to", peer_id},
        {"data", {
            {"message", request.message},
            {"audio", request.audio},
            {"video", request.video}
        }},
        {"time", GetCurrentTimestamp()}
    };

    LOG_INFO("发送连接请求到 {}", peer_id);
    return SendJsonMessage(msg.dump());
}

std::string SignalingClient::GetPeerDeviceId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peer_device_id_;
}

std::string SignalingClient::GetRole() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return role_;
}

RoomInfo SignalingClient::GetRoomInfo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return room_info_;
}

void SignalingClient::OnStatusChanged(SignalingStatusCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    status_callback_ = std::move(callback);
}

void SignalingClient::OnError(SignalingErrorCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    error_callback_ = std::move(callback);
}

void SignalingClient::OnOfferReceived(OfferReceivedCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    offer_callback_ = std::move(callback);
}

void SignalingClient::OnAnswerReceived(AnswerReceivedCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    answer_callback_ = std::move(callback);
}

void SignalingClient::OnIceCandidateReceived(IceCandidateCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    ice_callback_ = std::move(callback);
}

void SignalingClient::OnConnectionRequest(ConnectionRequestCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    conn_request_callback_ = std::move(callback);
}

void SignalingClient::OnConnectionResponse(ConnectionResponseCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    conn_response_callback_ = std::move(callback);
}

void SignalingClient::OnRoomInfoChanged(RoomInfoCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    room_info_callback_ = std::move(callback);
}

void SignalingClient::OnWebRTCReady(WebRTCReadyCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    webrtc_ready_callback_ = std::move(callback);
}

const char* SignalingClient::StatusToString(SignalingStatus status) {
    switch (status) {
        case SignalingStatus::kDisconnected: return "Disconnected";
        case SignalingStatus::kConnecting: return "Connecting";
        case SignalingStatus::kConnected: return "Connected";
        case SignalingStatus::kJoined: return "Joined";
        case SignalingStatus::kPaired: return "Paired";
        default: return "Unknown";
    }
}

const char* SignalingClient::ErrorToString(SignalingError error) {
    switch (error) {
        case SignalingError::kNone: return "None";
        case SignalingError::kConnectionFailed: return "ConnectionFailed";
        case SignalingError::kSendFailed: return "SendFailed";
        case SignalingError::kRoomFull: return "RoomFull";
        case SignalingError::kRoomNotExists: return "RoomNotExists";
        case SignalingError::kTimeout: return "Timeout";
        case SignalingError::kPeerOffline: return "PeerOffline";
        case SignalingError::kServerError: return "ServerError";
        default: return "Unknown";
    }
}

// ============================================================================
// 私有方法实现
// ============================================================================

void SignalingClient::HandleOpen() {
    LOG_INFO("WebSocket 已连接");
    SetStatus(SignalingStatus::kConnected);
}

void SignalingClient::HandleClose() {
    LOG_INFO("WebSocket 已关闭");
    SetStatus(SignalingStatus::kDisconnected);
}

void SignalingClient::HandleError(const std::string& error) {
    LOG_ERROR("WebSocket 错误: {}", error);
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (error_callback_) {
        error_callback_(SignalingError::kConnectionFailed, error);
    }
}

void SignalingClient::HandleMessage(const std::string& message) {
    try {
        auto msg = json::parse(message);
        std::string type = msg.value("type", "");
        
        LOG_DEBUG("收到消息: type={}", type);

        if (type == "role") {
            // 配对成功，收到角色分配
            std::string role = msg.value("data", json::object()).value("role", "");
            std::string peer_id = msg.value("from", "");
            HandleRoleMessage(role, peer_id);
        } else if (type == "offer") {
            std::string from = msg.value("from", "");
            std::string sdp = msg.value("data", json::object()).value("sdp", "");
            HandleOfferMessage(from, sdp);
        } else if (type == "answer") {
            std::string from = msg.value("from", "");
            std::string sdp = msg.value("data", json::object()).value("sdp", "");
            HandleAnswerMessage(from, sdp);
        } else if (type == "ice") {
            std::string from = msg.value("from", "");
            auto data = msg.value("data", json::object());
            std::string candidate = data.value("candidate", "");
            std::string mid = data.value("sdpMid", "");
            int mline_index = data.value("sdpMLineIndex", 0);
            HandleIceMessage(from, candidate, mid, mline_index);
        } else if (type == "info") {
            auto data = msg.value("data", json::object());
            RoomInfo info;
            info.room_id = data.value("roomId", "");
            info.num = data.value("num", 0);
            HandleInfoMessage(info);
        } else if (type == "error") {
            auto data = msg.value("data", json::object());
            int code = data.value("code", 0);
            std::string err_msg = data.value("message", "Unknown error");
            HandleErrorMessage(code, err_msg);
        } else if (type == "get_connect") {
            auto data = msg.value("data", json::object());
            ConnectionRequest req;
            req.message = data.value("message", true);
            req.audio = data.value("audio", false);
            req.video = data.value("video", true);
            HandleConnectionRequest(req);
        } else if (type == "connect_response") {
            auto data = msg.value("data", json::object());
            bool accepted = data.value("accepted", false);
            HandleConnectionResponse(accepted);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("解析消息失败: {}", e.what());
    }
}

void SignalingClient::HandleRoleMessage(const std::string& role, const std::string& peer_id) {
    LOG_INFO("配对成功: role={}, peer={}", role, peer_id);
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        role_ = role;
        peer_device_id_ = peer_id;
    }
    
    SetStatus(SignalingStatus::kPaired);
    
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (webrtc_ready_callback_) {
        webrtc_ready_callback_(role, peer_id);
    }
}

void SignalingClient::HandleOfferMessage(const std::string& from, const std::string& sdp) {
    LOG_INFO("收到 Offer from {}", from);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        peer_device_id_ = from;
    }
    
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (offer_callback_) {
        offer_callback_(sdp);
    }
}

void SignalingClient::HandleAnswerMessage(const std::string& from, const std::string& sdp) {
    LOG_INFO("收到 Answer from {}", from);
    
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (answer_callback_) {
        answer_callback_(sdp);
    }
}

void SignalingClient::HandleIceMessage(const std::string& from, const std::string& candidate,
                                        const std::string& mid, int mline_index) {
    LOG_DEBUG("收到 ICE 候选 from {}", from);
    
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (ice_callback_) {
        ice_callback_(candidate, mid, mline_index);
    }
}

void SignalingClient::HandleInfoMessage(const RoomInfo& info) {
    LOG_INFO("房间信息: room={}, num={}", info.room_id, info.num);
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        room_info_ = info;
    }
    
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (room_info_callback_) {
        room_info_callback_(info);
    }
}

void SignalingClient::HandleErrorMessage(int code, const std::string& message) {
    LOG_ERROR("信令错误: code={}, msg={}", code, message);
    
    SignalingError error = SignalingError::kServerError;
    switch (code) {
        case ERROR_CODE_ROOM_FULL: error = SignalingError::kRoomFull; break;
        case ERROR_CODE_ROOM_NOT_EXISTS: error = SignalingError::kRoomNotExists; break;
        case ERROR_CODE_CONNECTION_TIMEOUT: error = SignalingError::kTimeout; break;
        case ERROR_CODE_PEER_OFFLINE: error = SignalingError::kPeerOffline; break;
    }
    
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (error_callback_) {
        error_callback_(error, message);
    }
}

void SignalingClient::HandleConnectionRequest(const ConnectionRequest& request) {
    LOG_INFO("收到连接请求: message={}, audio={}, video={}", 
             request.message, request.audio, request.video);
    
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (conn_request_callback_) {
        conn_request_callback_(request);
    }
}

void SignalingClient::HandleConnectionResponse(bool accepted) {
    LOG_INFO("收到连接响应: accepted={}", accepted);
    
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (conn_response_callback_) {
        conn_response_callback_(accepted);
    }
}

bool SignalingClient::SendJsonMessage(const std::string& json_str) {
    if (!ws_ || !ws_->isOpen()) {
        LOG_ERROR("WebSocket 未连接");
        return false;
    }

    try {
        ws_->send(json_str);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("发送消息失败: {}", e.what());
        return false;
    }
}

void SignalingClient::SetStatus(SignalingStatus new_status) {
    auto old_status = status_.exchange(new_status);
    if (old_status != new_status) {
        LOG_INFO("信令状态: {} -> {}", StatusToString(old_status), StatusToString(new_status));
        
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (status_callback_) {
            status_callback_(new_status);
        }
    }
}

std::string SignalingClient::ExtractRoomId(const std::string& device_id) const {
    // 从 device_id 提取房间 ID（例如：glasses_123456 -> 123456）
    auto pos = device_id.find('_');
    if (pos != std::string::npos && pos + 1 < device_id.length()) {
        return device_id.substr(pos + 1);
    }
    return device_id;
}

uint64_t SignalingClient::GetCurrentTimestamp() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

