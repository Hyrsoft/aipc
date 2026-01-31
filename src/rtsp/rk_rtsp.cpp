/**
 * @file rk_rtsp.cpp
 * @brief RTSP 服务器实现
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

// 定义模块名，必须在 #include "logger.h" 之前
#define LOG_TAG "rtsp"

#include "rk_rtsp.h"
#include "rtsp_demo.h"
#include "common/logger.h"

#include "rk_mpi_mb.h"
#include <unistd.h>

// ============================================================================
// RtspServer 类实现
// ============================================================================

RtspServer::RtspServer() = default;

RtspServer::~RtspServer() {
    Deinit();
}

bool RtspServer::Init(const RtspConfig& config) {
    if (initialized_) {
        LOG_WARN("RTSP server already initialized");
        return true;
    }

    config_ = config;

    // 创建 RTSP demo（带重试机制，等待端口释放）
    const int maxRetries = 10;
    const int retryDelayMs = 500;
    
    for (int retry = 0; retry < maxRetries; ++retry) {
        demo_handle_ = create_rtsp_demo(config_.port);
        if (demo_handle_) {
            break;  // 成功
        }
        
        if (retry < maxRetries - 1) {
            LOG_WARN("Port {} may be in use, retrying ({}/{})...", 
                     config_.port, retry + 1, maxRetries);
            usleep(retryDelayMs * 1000);
        }
    }
    
    if (!demo_handle_) {
        LOG_ERROR("Failed to create RTSP demo on port {} after {} retries", 
                  config_.port, maxRetries);
        return false;
    }

    // 创建 RTSP session
    session_handle_ = rtsp_new_session(
        static_cast<rtsp_demo_handle>(demo_handle_), 
        config_.path.c_str()
    );
    if (!session_handle_) {
        LOG_ERROR("Failed to create RTSP session for path: {}", config_.path);
        rtsp_del_demo(static_cast<rtsp_demo_handle>(demo_handle_));
        demo_handle_ = nullptr;
        return false;
    }

    // 设置视频编码格式
    int codecId = (config_.codecType == 2) ? RTSP_CODEC_ID_VIDEO_H265 : RTSP_CODEC_ID_VIDEO_H264;
    rtsp_set_video(
        static_cast<rtsp_session_handle>(session_handle_), 
        codecId, 
        nullptr, 
        0
    );

    // 同步时间戳
    SyncVideoTimestamp();

    initialized_ = true;
    stats_ = Stats{};  // 重置统计

    LOG_INFO("RTSP server initialized on port {}, path: {}", config_.port, config_.path);
    return true;
}

void RtspServer::Deinit() {
    if (!initialized_) {
        return;
    }

    initialized_ = false;

    if (session_handle_) {
        rtsp_del_session(static_cast<rtsp_session_handle>(session_handle_));
        session_handle_ = nullptr;
    }

    if (demo_handle_) {
        rtsp_del_demo(static_cast<rtsp_demo_handle>(demo_handle_));
        demo_handle_ = nullptr;
    }

    LOG_INFO("RTSP server deinitialized, stats: {} frames, {} bytes sent, {} errors",
             stats_.framesSent, stats_.bytesSent, stats_.errors);
}

bool RtspServer::SendVideoFrame(const EncodedStreamPtr& stream) {
    if (!initialized_ || !stream || !stream->pstPack) {
        return false;
    }

    // 从 MPI buffer 获取虚拟地址
    void* data = RK_MPI_MB_Handle2VirAddr(stream->pstPack->pMbBlk);
    if (!data) {
        LOG_ERROR("Failed to get virtual address from MB handle");
        stats_.errors++;
        return false;
    }

    return SendVideoData(
        static_cast<const uint8_t*>(data),
        stream->pstPack->u32Len,
        stream->pstPack->u64PTS
    );
}

bool RtspServer::SendVideoData(const uint8_t* data, int len, uint64_t pts) {
    if (!initialized_ || !data || len <= 0) {
        return false;
    }

    int ret = rtsp_tx_video(
        static_cast<rtsp_session_handle>(session_handle_),
        data,
        len,
        pts
    );

    if (ret < 0) {
        stats_.errors++;
        return false;
    }

    // 处理 RTSP 事件
    DoEvent();

    stats_.framesSent++;
    stats_.bytesSent += len;

    return true;
}

void RtspServer::DoEvent() {
    if (demo_handle_) {
        rtsp_do_event(static_cast<rtsp_demo_handle>(demo_handle_));
    }
}

void RtspServer::SyncVideoTimestamp() {
    if (session_handle_) {
        rtsp_sync_video_ts(
            static_cast<rtsp_session_handle>(session_handle_),
            rtsp_get_reltime(),
            rtsp_get_ntptime()
        );
    }
}

std::string RtspServer::GetUrl() const {
    // 返回 RTSP URL 格式
    return "rtsp://<device_ip>:" + std::to_string(config_.port) + config_.path;
}

// ============================================================================
// 全局实例
// ============================================================================

static RtspServer g_rtspServer;

RtspServer& GetRtspServer() {
    return g_rtspServer;
}

// ============================================================================
// 便捷函数实现
// ============================================================================

bool rtsp_server_init(const RtspConfig& config) {
    return GetRtspServer().Init(config);
}

void rtsp_server_deinit() {
    GetRtspServer().Deinit();
}

void rtsp_stream_consumer(EncodedStreamPtr stream, void* /* userData */) {
    GetRtspServer().SendVideoFrame(stream);
}