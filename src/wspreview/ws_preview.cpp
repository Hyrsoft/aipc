/**
 * @file ws_preview.cpp
 * @brief WebSocket H.264 裸流预览实现
 */

#include "ws_preview.h"
#include "common/logger.h"

// RK MPI headers for VENC stream access
#include "rk_mpi_mb.h"
#include "rk_comm_venc.h"  // for VENC_STREAM_S

#include <rtc/websocketserver.hpp>
#include <rtc/websocket.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

#undef LOG_TAG
#define LOG_TAG "ws_preview"

// ============================================================================
// WsPreviewServer 实现
// ============================================================================

WsPreviewServer::WsPreviewServer(const WsPreviewConfig& config)
    : config_(config) {
    LOG_INFO("WebSocket 预览服务器创建: port={}", config_.port);
}

WsPreviewServer::~WsPreviewServer() {
    Stop();
    LOG_INFO("WebSocket 预览服务器销毁");
}

bool WsPreviewServer::Start() {
    if (running_.load()) {
        LOG_WARN("WebSocket 预览服务器已在运行");
        return true;
    }

    // 尝试手动创建 socket 测试
    LOG_INFO("尝试创建 WebSocket 服务器，端口: {}", config_.port);
    
    // 重试机制
    const int maxRetries = 3;
    const int retryDelayMs = 1000;

    for (int retry = 0; retry < maxRetries; ++retry) {
        try {
            // 创建 WebSocket 服务器配置
            rtc::WebSocketServer::Configuration config;
            config.port = config_.port;
            config.enableTls = false;  // 内网使用，不需要 TLS
            config.bindAddress = "0.0.0.0";  // 明确绑定 IPv4

            LOG_DEBUG("创建 WebSocketServer 对象...");
            // 创建服务器
            ws_server_ = std::make_unique<rtc::WebSocketServer>(config);

            // 设置客户端连接回调
            ws_server_->onClient([this](std::shared_ptr<rtc::WebSocket> ws) {
                OnClientConnected(ws);
            });

            running_.store(true);
            LOG_INFO("WebSocket 预览服务器启动成功: port={}", ws_server_->port());
            return true;

        } catch (const std::exception& e) {
            if (retry < maxRetries - 1) {
                LOG_WARN("WebSocket 端口 {} 创建失败，重试中 ({}/{})...: {}", 
                         config_.port, retry + 1, maxRetries, e.what());
                std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
            } else {
                LOG_ERROR("启动 WebSocket 服务器失败 (重试 {} 次): {}", maxRetries, e.what());
            }
        }
    }

    return false;
}

void WsPreviewServer::Stop() {
    if (!running_.load()) {
        return;
    }

    LOG_INFO("停止 WebSocket 预览服务器...");
    running_.store(false);

    // 关闭所有客户端连接
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& weak_ws : clients_) {
            if (auto ws = weak_ws.lock()) {
                try {
                    ws->close();
                } catch (...) {}
            }
        }
        clients_.clear();
    }

    // 停止服务器
    if (ws_server_) {
        ws_server_->stop();
        ws_server_.reset();
    }

    LOG_INFO("WebSocket 预览服务器已停止, 总计发送: {} 帧, {} 字节",
             frames_sent_.load(), bytes_sent_.load());
}

uint16_t WsPreviewServer::GetPort() const {
    return ws_server_ ? ws_server_->port() : 0;
}

size_t WsPreviewServer::GetClientCount() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    
    // 清理已断开的客户端并计数
    size_t count = 0;
    for (const auto& weak_ws : clients_) {
        if (auto ws = weak_ws.lock()) {
            if (ws->isOpen()) {
                count++;
            }
        }
    }
    return count;
}

void WsPreviewServer::OnClientConnected(std::shared_ptr<rtc::WebSocket> ws) {
    LOG_INFO("WebSocket 客户端连接, path={}", ws->path().value_or("(none)"));

    // 先添加到客户端列表（保持引用）
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        
        // 清理已断开的连接
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(),
                [](const std::weak_ptr<rtc::WebSocket>& w) {
                    auto s = w.lock();
                    return !s || !s->isOpen();
                }),
            clients_.end());

        // 检查是否超过最大连接数
        if (static_cast<int>(clients_.size()) >= config_.max_clients) {
            LOG_WARN("达到最大客户端数量限制 ({}), 拒绝新连接", config_.max_clients);
            ws->close();
            return;
        }

        clients_.push_back(ws);
        LOG_INFO("当前客户端数量: {}", clients_.size());
    }

    // 设置 onOpen 回调 - WebSocket 完全就绪后再发送数据
    ws->onOpen([this, weak_ws = std::weak_ptr<rtc::WebSocket>(ws)]() {
        LOG_INFO("WebSocket 客户端已就绪");
        if (auto ws = weak_ws.lock()) {
            // 发送缓存的 SPS/PPS
            SendSpsPps(ws);
        }
    });

    // 设置消息回调（可用于接收客户端控制命令）
    ws->onMessage([](auto data) {
        // 目前不处理客户端消息
        if (std::holds_alternative<std::string>(data)) {
            LOG_DEBUG("收到客户端消息: {}", std::get<std::string>(data));
        }
    });

    ws->onClosed([this]() {
        LOG_INFO("WebSocket 客户端断开");
        // 清理断开的连接
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(),
                [](const std::weak_ptr<rtc::WebSocket>& w) {
                    auto s = w.lock();
                    return !s || !s->isOpen();
                }),
            clients_.end());
    });

    ws->onError([](std::string error) {
        LOG_WARN("WebSocket 错误: {}", error);
    });
}

void WsPreviewServer::SendVideoFrame(const uint8_t* data, size_t size, uint64_t /*timestamp*/) {
    if (!running_.load() || !data || size == 0) {
        return;
    }

    // 提取 SPS/PPS（如果有）
    ExtractSpsPps(data, size);

    // 获取活跃客户端
    std::vector<std::shared_ptr<rtc::WebSocket>> active_clients;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& weak_ws : clients_) {
            if (auto ws = weak_ws.lock()) {
                if (ws->isOpen()) {
                    active_clients.push_back(ws);
                }
            }
        }
    }

    if (active_clients.empty()) {
        return;
    }

    // 发送给所有客户端
    for (auto& ws : active_clients) {
        try {
            ws->send(reinterpret_cast<const std::byte*>(data), size);
        } catch (const std::exception& e) {
            LOG_DEBUG("发送视频帧失败: {}", e.what());
        }
    }

    frames_sent_++;
    bytes_sent_ += size * active_clients.size();
}

void WsPreviewServer::ExtractSpsPps(const uint8_t* data, size_t size) {
    // 搜索 NAL 单元，提取 SPS (type=7) 和 PPS (type=8)
    for (size_t i = 0; i + 4 < size; ) {
        // 查找起始码
        size_t start_code_len = 0;
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
            start_code_len = 4;
        } else if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) {
            start_code_len = 3;
        } else {
            i++;
            continue;
        }

        size_t nal_start = i + start_code_len;
        if (nal_start >= size) break;

        uint8_t nal_type = data[nal_start] & 0x1F;

        // 查找下一个起始码以确定 NAL 单元结束位置
        size_t nal_end = size;
        for (size_t j = nal_start + 1; j + 3 < size; j++) {
            if ((data[j] == 0 && data[j+1] == 0 && data[j+2] == 0 && data[j+3] == 1) ||
                (data[j] == 0 && data[j+1] == 0 && data[j+2] == 1)) {
                nal_end = j;
                break;
            }
        }

        size_t nal_size = nal_end - i;  // 包含起始码

        if (nal_type == 7) {  // SPS
            std::lock_guard<std::mutex> lock(sps_pps_mutex_);
            cached_sps_.assign(data + i, data + i + nal_size);
            LOG_DEBUG("缓存 SPS: {} 字节", nal_size);
        } else if (nal_type == 8) {  // PPS
            std::lock_guard<std::mutex> lock(sps_pps_mutex_);
            cached_pps_.assign(data + i, data + i + nal_size);
            LOG_DEBUG("缓存 PPS: {} 字节", nal_size);
        }

        i = nal_end;
    }
}

bool WsPreviewServer::IsKeyframe(const uint8_t* data, size_t size) const {
    for (size_t i = 0; i + 4 < size; i++) {
        if ((data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) ||
            (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1)) {
            size_t nal_start = (data[i+2] == 1) ? i + 3 : i + 4;
            if (nal_start < size) {
                uint8_t nal_type = data[nal_start] & 0x1F;
                // IDR 帧 (type=5) 或 SPS (type=7)
                if (nal_type == 5 || nal_type == 7) {
                    return true;
                }
            }
        }
    }
    return false;
}

void WsPreviewServer::SendSpsPps(std::shared_ptr<rtc::WebSocket> ws) {
    std::lock_guard<std::mutex> lock(sps_pps_mutex_);

    if (cached_sps_.empty() || cached_pps_.empty()) {
        LOG_DEBUG("SPS/PPS 尚未缓存，等待下一个关键帧");
        return;
    }

    try {
        // 先发送 SPS
        ws->send(reinterpret_cast<const std::byte*>(cached_sps_.data()), cached_sps_.size());
        // 再发送 PPS
        ws->send(reinterpret_cast<const std::byte*>(cached_pps_.data()), cached_pps_.size());
        LOG_DEBUG("已发送缓存的 SPS/PPS 给新客户端");
    } catch (const std::exception& e) {
        LOG_WARN("发送 SPS/PPS 失败: {}", e.what());
    }
}

void WsPreviewServer::StreamConsumer(EncodedStreamPtr stream, void* user_data) {
    auto* self = static_cast<WsPreviewServer*>(user_data);
    if (!self || !stream) {
        return;
    }

    if (!stream->pstPack) {
        return;
    }

    // 获取视频数据
    const uint8_t* data = static_cast<const uint8_t*>(
        RK_MPI_MB_Handle2VirAddr(stream->pstPack->pMbBlk));
    uint32_t len = stream->pstPack->u32Len;
    uint64_t pts = stream->pstPack->u64PTS;

    if (data && len > 0) {
        self->SendVideoFrame(data, len, pts);
    }
}

// ============================================================================
// 全局实例管理
// ============================================================================

static std::unique_ptr<WsPreviewServer> g_ws_preview_server;

WsPreviewServer* GetWsPreviewServer() {
    return g_ws_preview_server.get();
}

void CreateWsPreviewServer(const WsPreviewConfig& config) {
    if (!g_ws_preview_server) {
        g_ws_preview_server = std::make_unique<WsPreviewServer>(config);
    }
}

void DestroyWsPreviewServer() {
    g_ws_preview_server.reset();
}
