/**
 * @file webrtc_service.h
 * @brief WebRTC 推流服务 - 管理 WebRTC 连接和推流逻辑
 *
 * 与 RTSP 和 File 模块并列，作为视频流的第三条推送路径。
 * 
 * 提供：
 * - WebRTC 连接管理
 * - 视频流推送到浏览器
 * - 与 StreamDispatcher 集成
 *
 * 作为视频编码流的消费者之一，使用 AsyncIO 模式，
 * 网络发送通过 asio::post 异步执行。
 *
 * @author 好软，好温暖
 * @date 2026-02-04
 */

#pragma once

#include <string>
#include <memory>
#include <atomic>

#include "webrtc.h"
#include "signaling.h"
#include "common/media_buffer.h"

// ============================================================================
// WebRTC 服务配置
// ============================================================================

struct WebRTCServiceConfig {
    // 信令配置
    std::string device_id = "camera_000001";      ///< 设备 ID
    std::string signaling_url;                     ///< 信令服务器地址

    // WebRTC 配置
    WebRTCConfig webrtc_config;                    ///< WebRTC 详细配置
};

// ============================================================================
// WebRTC 服务类
// ============================================================================

/**
 * @brief WebRTC 推流服务
 * 
 * 管理 WebRTC 连接和推流的生命周期
 * RAII 设计：构造时初始化，析构时清理
 */
class WebRTCService {
public:
    /**
     * @brief 构造函数
     * @param config WebRTC 服务配置
     */
    explicit WebRTCService(const WebRTCServiceConfig& config);
    
    ~WebRTCService();

    // 禁用拷贝
    WebRTCService(const WebRTCService&) = delete;
    WebRTCService& operator=(const WebRTCService&) = delete;

    /**
     * @brief 启动 WebRTC 服务
     * @return true 成功，false 失败
     */
    bool Start();

    /**
     * @brief 停止 WebRTC 服务
     */
    void Stop();

    /**
     * @brief 检查是否已初始化
     */
    bool IsValid() const { return valid_; }

    /**
     * @brief 检查是否正在运行
     */
    bool IsRunning() const { return valid_; }

    /**
     * @brief 检查是否已连接
     */
    bool IsConnected() const;

    /**
     * @brief 获取当前状态
     */
    WebRTCState GetState() const;

    /**
     * @brief 获取统计信息
     */
    WebRTCStats GetStats() const;

    /**
     * @brief 流消费者回调（用于注册到 StreamDispatcher）
     */
    static void StreamConsumer(EncodedStreamPtr stream, void* user_data);

    /**
     * @brief 发送视频帧
     * @param stream 编码后的视频流
     */
    void SendVideoFrame(const EncodedStreamPtr& stream);

    // ========================================================================
    // 回调设置
    // ========================================================================

    /**
     * @brief 设置状态变化回调
     */
    void OnStateChanged(StateCallback callback);

    /**
     * @brief 设置错误回调
     */
    void OnError(ErrorCallback callback);

    // ========================================================================
    // HTTP 信令模式 API
    // ========================================================================

    /**
     * @brief 创建 Offer (HTTP 信令模式)
     * @return SDP Offer
     */
    std::string CreateOfferForHttp();

    /**
     * @brief 设置 Answer (HTTP 信令模式)
     */
    bool SetAnswerFromHttp(const std::string& sdp);

    /**
     * @brief 添加远程 ICE 候选 (HTTP 信令模式)
     */
    bool AddIceCandidateFromHttp(const std::string& candidate, const std::string& mid);

    /**
     * @brief 获取本地 ICE 候选列表
     */
    std::vector<std::pair<std::string, std::string>> GetLocalIceCandidates();

private:
    WebRTCServiceConfig config_;
    bool valid_ = false;
    
    std::shared_ptr<SignalingClient> signaling_;
    std::shared_ptr<WebRTCSystem> webrtc_;
};

// ============================================================================
// 全局实例管理
// ============================================================================

/**
 * @brief 获取全局 WebRTC 服务实例
 */
WebRTCService* GetWebRTCService();

/**
 * @brief 创建全局 WebRTC 服务实例
 * @param config WebRTC 服务配置
 */
void CreateWebRTCService(const WebRTCServiceConfig& config);

/**
 * @brief 销毁全局 WebRTC 服务实例
 */
void DestroyWebRTCService();

