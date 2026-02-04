/**
 * @file stream_manager.h
 * @brief 视频流分发管理 - 协调各个流输出路径
 *
 * 负责：
 * - 管理流分发器（StreamDispatcher）
 * - 协调 RTSP、File、WebRTC、WsPreview 等消费者的注册
 * - 提供统一的启动/停止接口
 *
 * 各个消费者的具体实现在各自的模块中：
 * - rtsp/rtsp_service.h
 * - file/file_service.h
 * - webrtc/webrtc_service.h
 * - wspreview/ws_preview.h
 *
 * @author 好软，好温暖
 * @date 2026-02-04
 */

#pragma once

#include <string>
#include "rtsp/rk_rtsp.h"
#include "file/file_saver.h"
#include "webrtc/webrtc_service.h"
#include "wspreview/ws_preview.h"

// 前向声明，避免头文件依赖
class RtspService;
class FileService;
class WebRTCService;
class WsPreviewServer;

// ============================================================================
// 流输出配置
// ============================================================================

/**
 * @brief 流输出总配置
 */
struct StreamConfig {
    bool enable_rtsp = true;           ///< 是否启用 RTSP 推流
    bool enable_file = false;          ///< 是否启用文件保存
    bool enable_webrtc = false;        ///< 是否启用 WebRTC
    bool enable_ws_preview = false;    ///< 是否启用 WebSocket 预览
    
    RtspConfig rtsp_config;            ///< RTSP 配置
    Mp4RecordConfig mp4_config;        ///< MP4 录制配置
    WebRTCServiceConfig webrtc_config;  ///< WebRTC 配置
    WsPreviewConfig ws_preview_config; ///< WebSocket 预览配置
};

// ============================================================================
// 流管理器
// ============================================================================

/**
 * @brief 流管理器
 * 
 * 协调各个流输出路径，管理它们的生命周期
 * 提供统一的控制接口
 */
class StreamManager {
public:
    /**
     * @brief 构造函数
     * @param config 流配置
     */
    explicit StreamManager(const StreamConfig& config);
    
    ~StreamManager();

    // 禁用拷贝
    StreamManager(const StreamManager&) = delete;
    StreamManager& operator=(const StreamManager&) = delete;

    /**
     * @brief 启动所有已注册的流输出
     */
    void Start();

    /**
     * @brief 停止所有流输出
     */
    void Stop();

    /**
     * @brief 检查是否正在运行
     */
    bool IsRunning() const { return running_; }

    // ========================================================================
    // 访问各个子模块
    // ========================================================================

    RtspService* GetRtspService() const { return rtsp_service_.get(); }
    FileService* GetFileService() const { return file_service_.get(); }
    WebRTCService* GetWebRTCService() const { return webrtc_service_.get(); }
    WsPreviewServer* GetWsPreviewServer() const { return ws_preview_server_.get(); }

private:
    StreamConfig config_;
    bool running_ = false;

    std::unique_ptr<RtspService> rtsp_service_;
    std::unique_ptr<FileService> file_service_;
    std::unique_ptr<WebRTCService> webrtc_service_;
    std::unique_ptr<WsPreviewServer> ws_preview_server_;
};

// ============================================================================
// 全局流管理器
// ============================================================================

/**
 * @brief 获取全局流管理器实例
 */
StreamManager* GetStreamManager();

/**
 * @brief 创建全局流管理器
 * @param config 流配置
 */
void CreateStreamManager(const StreamConfig& config);

/**
 * @brief 销毁全局流管理器
 */
void DestroyStreamManager();

