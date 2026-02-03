/**
 * @file http_server.h
 * @brief HTTP 服务器封装 - 基于 cpp-httplib
 *
 * 提供 HTTP 服务器功能，支持静态文件服务和 API 接口
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

// 前向声明 httplib 类型
namespace httplib {
class Server;
class Request;
class Response;
}  // namespace httplib

// ============================================================================
// HTTP 服务器配置
// ============================================================================

struct HttpServerConfig {
    std::string host = "0.0.0.0";     ///< 监听地址
    int port = 8080;                   ///< 监听端口
    std::string static_dir = "";       ///< 静态文件目录（空则不提供静态文件服务）
    std::string static_mount = "/";    ///< 静态文件挂载路径
    int thread_pool_size = 1;          ///< 线程池大小
};

// ============================================================================
// HTTP 请求/响应类型定义
// ============================================================================

using HttpRequest = httplib::Request;
using HttpResponse = httplib::Response;
using HttpHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

// ============================================================================
// HTTP 服务器类
// ============================================================================

/**
 * @brief HTTP 服务器封装类
 *
 * 封装 cpp-httplib，提供面向对象的接口
 *
 * 使用方式：
 * 1. 创建实例并配置
 * 2. 注册 API 路由
 * 3. 调用 Start() 启动服务器（在独立线程中运行）
 * 4. 调用 Stop() 停止服务器
 */
class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    // 禁用拷贝
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /**
     * @brief 初始化 HTTP 服务器
     *
     * @param config HTTP 配置参数
     * @return true 成功，false 失败
     */
    bool Init(const HttpServerConfig& config = HttpServerConfig{});

    /**
     * @brief 启动 HTTP 服务器（非阻塞）
     *
     * @return true 成功，false 失败
     */
    bool Start();

    /**
     * @brief 停止 HTTP 服务器
     */
    void Stop();

    /**
     * @brief 检查服务器是否正在运行
     *
     * @return true 运行中，false 已停止
     */
    bool IsRunning() const;

    // ========================================
    // 路由注册接口
    // ========================================

    /**
     * @brief 注册 GET 请求处理器
     *
     * @param pattern URL 模式（支持正则）
     * @param handler 处理函数
     */
    void Get(const std::string& pattern, HttpHandler handler);

    /**
     * @brief 注册 POST 请求处理器
     *
     * @param pattern URL 模式
     * @param handler 处理函数
     */
    void Post(const std::string& pattern, HttpHandler handler);

    /**
     * @brief 注册 PUT 请求处理器
     *
     * @param pattern URL 模式
     * @param handler 处理函数
     */
    void Put(const std::string& pattern, HttpHandler handler);

    /**
     * @brief 注册 DELETE 请求处理器
     *
     * @param pattern URL 模式
     * @param handler 处理函数
     */
    void Delete(const std::string& pattern, HttpHandler handler);

    // ========================================
    // 静态文件服务
    // ========================================

    /**
     * @brief 设置静态文件目录
     *
     * @param mount_point 挂载点（URL 路径）
     * @param dir 本地目录路径
     * @return true 成功，false 失败
     */
    bool SetStaticFileDir(const std::string& mount_point, const std::string& dir);

    // ========================================
    // 辅助方法
    // ========================================

    /**
     * @brief 获取服务器监听地址
     *
     * @return 监听地址字符串 (host:port)
     */
    std::string GetListenAddress() const;

private:
    /**
     * @brief 服务器线程入口
     */
    void ServerThread();

    std::unique_ptr<httplib::Server> server_;  ///< httplib 服务器实例
    std::thread server_thread_;                 ///< 服务器线程
    std::atomic<bool> running_{false};          ///< 运行状态标志
    HttpServerConfig config_;                   ///< 服务器配置
};
