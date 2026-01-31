/**
 * @file rk_rtsp.h
 * @brief RTSP 服务器封装 - 基于 Rockchip rtsp_demo 库
 *
 * 提供 RTSP 服务器的初始化、推流、状态查询等功能
 * 支持 H.264/H.265 视频流推送
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#pragma once

#include <cstdint>
#include <string>
#include <atomic>
#include <memory>

// RKMPI 头文件
#include "rk_mpi_venc.h"

// 前向声明编码流智能指针类型
using EncodedStreamPtr = std::shared_ptr<VENC_STREAM_S>;

// ============================================================================
// RTSP 配置参数
// ============================================================================

struct RtspConfig {
    int port = 554;                   ///< RTSP 服务端口
    std::string path = "/live/0";     ///< 推流路径
    int codecType = 1;                ///< 编码类型：1=H.264, 2=H.265
};

// ============================================================================
// RTSP 服务器类
// ============================================================================

/**
 * @brief RTSP 服务器封装类
 * 
 * 封装 Rockchip rtsp_demo 库，提供面向对象的接口
 * 
 * 使用方式：
 * 1. 创建实例并调用 Init() 初始化
 * 2. 调用 SendVideoFrame() 发送编码后的视频帧
 * 3. 定期调用 DoEvent() 处理 RTSP 事件（或在 SendVideoFrame 中自动调用）
 */
class RtspServer {
public:
    RtspServer();
    ~RtspServer();

    // 禁用拷贝
    RtspServer(const RtspServer&) = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    /**
     * @brief 初始化 RTSP 服务器
     * 
     * @param config RTSP 配置参数
     * @return true 成功，false 失败
     */
    bool Init(const RtspConfig& config = RtspConfig{});

    /**
     * @brief 反初始化 RTSP 服务器
     */
    void Deinit();

    /**
     * @brief 发送编码后的视频帧
     * 
     * @param stream 编码流智能指针（来自 StreamDispatcher）
     * @return true 成功，false 失败
     */
    bool SendVideoFrame(const EncodedStreamPtr& stream);

    /**
     * @brief 发送原始视频数据
     * 
     * @param data 视频数据指针
     * @param len 数据长度
     * @param pts 时间戳
     * @return true 成功，false 失败
     */
    bool SendVideoData(const uint8_t* data, int len, uint64_t pts);

    /**
     * @brief 处理 RTSP 事件（客户端连接、断开等）
     * 
     * 如果设置了自动处理，则无需手动调用此函数
     */
    void DoEvent();

    /**
     * @brief 同步视频时间戳
     * 
     * 用于保持 RTSP 流的时间戳同步
     */
    void SyncVideoTimestamp();

    /**
     * @brief 检查服务器是否已初始化
     */
    bool IsInitialized() const { return initialized_; }

    /**
     * @brief 获取 RTSP URL
     * 
     * @return RTSP 播放地址（例如：rtsp://192.168.1.100:554/live/0）
     */
    std::string GetUrl() const;

    /**
     * @brief 获取推流统计信息
     */
    struct Stats {
        uint64_t framesSent = 0;      ///< 已发送帧数
        uint64_t bytesSent = 0;       ///< 已发送字节数
        uint64_t errors = 0;          ///< 发送错误次数
    };
    Stats GetStats() const { return stats_; }

private:
    void* demo_handle_ = nullptr;     ///< rtsp_demo_handle
    void* session_handle_ = nullptr;  ///< rtsp_session_handle
    
    std::atomic<bool> initialized_{false};
    RtspConfig config_;
    Stats stats_;
};

// ============================================================================
// 全局 RTSP 服务器实例（单例模式）
// ============================================================================

/**
 * @brief 获取全局 RTSP 服务器实例
 */
RtspServer& GetRtspServer();

// ============================================================================
// 便捷函数
// ============================================================================

/**
 * @brief 初始化全局 RTSP 服务器
 * 
 * @param config RTSP 配置
 * @return true 成功，false 失败
 */
bool rtsp_server_init(const RtspConfig& config = RtspConfig{});

/**
 * @brief 反初始化全局 RTSP 服务器
 */
void rtsp_server_deinit();

/**
 * @brief 获取 RTSP 流消费者回调
 * 
 * 用于注册到 StreamDispatcher 的回调函数
 * 
 * @param stream 编码流
 * @param userData 用户数据（未使用）
 */
void rtsp_stream_consumer(EncodedStreamPtr stream, void* userData);
