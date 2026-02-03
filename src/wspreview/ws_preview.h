/**
 * @file ws_preview.h
 * @brief WebSocket H.264 裸流预览模块
 *
 * 通过 WebSocket 直接推送 H.264 NAL 单元到浏览器
 * 浏览器使用 jMuxer 将裸流封装为 fMP4 并利用硬件解码播放
 *
 * 优势：
 * - 极低延迟 (100-300ms)
 * - 跨平台兼容性好 (H.264 + MSE)
 * - 实现简单，不需要复杂的信令
 *
 * @author 好软，好温暖
 * @date 2026-02-04
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// 前向声明
namespace rtc {
class WebSocketServer;
class WebSocket;
}  // namespace rtc

// VENC 流类型（避免循环依赖）
struct rkVENC_STREAM_S;
using EncodedStreamPtr = std::shared_ptr<rkVENC_STREAM_S>;

// ============================================================================
// 配置结构
// ============================================================================

/**
 * @brief WebSocket 预览配置
 */
struct WsPreviewConfig {
    uint16_t port = 8082;           ///< WebSocket 服务器端口
    int max_clients = 5;            ///< 最大客户端数量
    int keyframe_interval_ms = 100; ///< 关键帧缓存刷新间隔
};

// ============================================================================
// WebSocket 预览服务器
// ============================================================================

/**
 * @brief WebSocket H.264 预览服务器
 *
 * 接收来自 VENC 的 H.264 编码数据，直接通过 WebSocket 推送给浏览器
 * 
 * 特点：
 * - 自动缓存最新的 SPS/PPS，新客户端连接时立即发送
 * - 支持多客户端同时预览
 * - 低延迟设计
 */
class WsPreviewServer {
public:
    /**
     * @brief 构造函数
     * @param config 配置参数
     */
    explicit WsPreviewServer(const WsPreviewConfig& config = WsPreviewConfig{});
    
    ~WsPreviewServer();

    // 禁用拷贝
    WsPreviewServer(const WsPreviewServer&) = delete;
    WsPreviewServer& operator=(const WsPreviewServer&) = delete;

    /**
     * @brief 启动 WebSocket 服务器
     * @return true 成功，false 失败
     */
    bool Start();

    /**
     * @brief 停止服务器
     */
    void Stop();

    /**
     * @brief 检查是否正在运行
     */
    bool IsRunning() const { return running_.load(); }

    /**
     * @brief 获取监听端口
     */
    uint16_t GetPort() const;

    /**
     * @brief 获取当前连接的客户端数量
     */
    size_t GetClientCount() const;

    /**
     * @brief 发送视频帧给所有客户端
     * 
     * @param data H.264 NAL 数据（Annex-B 格式，带起始码）
     * @param size 数据大小
     * @param timestamp 时间戳（微秒）
     */
    void SendVideoFrame(const uint8_t* data, size_t size, uint64_t timestamp);

    // ========================================================================
    // StreamDispatcher 回调接口
    // ========================================================================

    /**
     * @brief 流消费者回调函数
     * 
     * 用于注册到 StreamDispatcher
     */
    static void StreamConsumer(EncodedStreamPtr stream, void* user_data);

private:
    /**
     * @brief 处理新客户端连接
     */
    void OnClientConnected(std::shared_ptr<rtc::WebSocket> ws);

    /**
     * @brief 从 NAL 数据中提取 SPS/PPS
     */
    void ExtractSpsPps(const uint8_t* data, size_t size);

    /**
     * @brief 检查是否为关键帧
     */
    bool IsKeyframe(const uint8_t* data, size_t size) const;

    /**
     * @brief 发送 SPS/PPS 给客户端
     */
    void SendSpsPps(std::shared_ptr<rtc::WebSocket> ws);

    WsPreviewConfig config_;
    std::atomic<bool> running_{false};

    // WebSocket 服务器
    std::unique_ptr<rtc::WebSocketServer> ws_server_;

    // 客户端管理
    mutable std::mutex clients_mutex_;
    std::vector<std::weak_ptr<rtc::WebSocket>> clients_;

    // SPS/PPS 缓存（用于新客户端连接时发送）
    mutable std::mutex sps_pps_mutex_;
    std::vector<uint8_t> cached_sps_;
    std::vector<uint8_t> cached_pps_;

    // 统计
    std::atomic<uint64_t> frames_sent_{0};
    std::atomic<uint64_t> bytes_sent_{0};
};

// ============================================================================
// 全局实例管理（可选）
// ============================================================================

WsPreviewServer* GetWsPreviewServer();
void CreateWsPreviewServer(const WsPreviewConfig& config);
void DestroyWsPreviewServer();
