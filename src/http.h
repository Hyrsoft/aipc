/**
 * @file http.h
 * @brief HTTP API 模块 - 提供 REST API 接口控制系统
 *
 * 支持的 API:
 * - GET  /api/status          获取系统状态
 * - GET  /api/rtsp/status     获取 RTSP 状态
 * - POST /api/webrtc/start    启动 WebRTC
 * - POST /api/webrtc/stop     停止 WebRTC
 * - GET  /api/record/status   获取录制状态
 * - POST /api/record/start    开始录制
 * - POST /api/record/stop     停止录制
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#pragma once

#include <memory>
#include <string>
#include "httpserver/http_server.h"
#include "stream_manager.h"

// ============================================================================
// HTTP API 配置
// ============================================================================

struct HttpApiConfig {
    std::string host = "0.0.0.0";      ///< 监听地址
    int port = 8080;                    ///< 监听端口
    std::string static_dir = "/app/www"; ///< 静态文件目录
    int thread_pool_size = 1;           ///< 线程池大小
};

// ============================================================================
// HTTP API 管理类
// ============================================================================

/**
 * @brief HTTP API 管理类
 * 
 * 封装 HTTP 服务器的初始化、路由设置和生命周期管理
 */
class HttpApi {
public:
    HttpApi();
    ~HttpApi();

    // 禁用拷贝
    HttpApi(const HttpApi&) = delete;
    HttpApi& operator=(const HttpApi&) = delete;

    /**
     * @brief 初始化 HTTP API 服务
     * @param config HTTP 配置
     * @param stream_config 流配置（用于 API 响应）
     * @return true 成功，false 失败
     */
    bool Init(const HttpApiConfig& config, const StreamConfig& stream_config);

    /**
     * @brief 启动 HTTP 服务
     * @return true 成功，false 失败
     */
    bool Start();

    /**
     * @brief 停止 HTTP 服务
     */
    void Stop();

    /**
     * @brief 检查是否正在运行
     */
    bool IsRunning() const;

    /**
     * @brief 获取监听端口
     */
    int GetPort() const { return config_.port; }

private:
    /**
     * @brief 设置 API 路由
     */
    void SetupRoutes();

    HttpApiConfig config_;
    StreamConfig stream_config_;
    std::unique_ptr<HttpServer> server_;
};
