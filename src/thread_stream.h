/**
 * @file thread_stream.h
 * @brief 视频流分发管理 - 协调各个流输出路径
 *
 * 负责：
 * - 管理流分发器（StreamDispatcher）
 * - 协调 RTSP、File、WebRTC 等消费者的注册
 * - 提供统一的启动/停止接口
 *
 * 各个消费者的具体实现在各自的模块中：
 * - rtsp/thread_rtsp.h
 * - file/thread_file.h
 * - webrtc/thread_webrtc.h（待实现）
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#pragma once

#include <string>
#include "rtsp/rk_rtsp.h"
#include "file/file_saver.h"

// 前向声明，避免头文件依赖
class RtspThread;
class FileThread;

// ============================================================================
// 流输出配置
// ============================================================================

/**
 * @brief 流输出总配置
 */
struct StreamConfig {
    bool enable_rtsp = true;           ///< 是否启用 RTSP 推流
    bool enable_file = false;          ///< 是否启用文件保存
    bool enable_webrtc = false;        ///< 是否启用 WebRTC（待实现）
    
    RtspConfig rtsp_config;            ///< RTSP 配置
    Mp4RecordConfig mp4_config;        ///< MP4 录制配置
    JpegCaptureConfig jpeg_config;     ///< JPEG 拍照配置
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

    RtspThread* GetRtspThread() const { return rtsp_thread_.get(); }
    FileThread* GetFileThread() const { return file_thread_.get(); }

private:
    StreamConfig config_;
    bool running_ = false;

    std::unique_ptr<RtspThread> rtsp_thread_;
    std::unique_ptr<FileThread> file_thread_;
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

