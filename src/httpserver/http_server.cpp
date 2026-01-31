/**
 * @file http_server.cpp
 * @brief HTTP 服务器实现
 */

#include "http_server.h"
#include "common/logger.h"

// cpp-httplib (header-only)
#include <httplib.h>

// ============================================================================
// HttpServer 实现
// ============================================================================

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() {
    Stop();
}

bool HttpServer::Init(const HttpServerConfig& config) {
    LOG_INFO("初始化 HTTP 服务器: {}:{}", config.host, config.port);

    config_ = config;

    // 创建 httplib 服务器实例
    server_ = std::make_unique<httplib::Server>();

    if (!server_) {
        LOG_ERROR("创建 HTTP 服务器实例失败");
        return false;
    }

    // 设置线程池大小
    server_->new_task_queue = [this] {
        return new httplib::ThreadPool(config_.thread_pool_size);
    };

    // 设置错误处理
    server_->set_error_handler([](const HttpRequest& req, HttpResponse& res) {
        LOG_WARN("HTTP 错误: {} {} -> {}", req.method, req.path, res.status);
        res.set_content("Error: " + std::to_string(res.status), "text/plain");
    });

    // 设置异常处理
    server_->set_exception_handler([](const HttpRequest& req, HttpResponse& res,
                                      std::exception_ptr ep) {
        try {
            if (ep) {
                std::rethrow_exception(ep);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("HTTP 异常: {} {} -> {}", req.method, req.path, e.what());
        }
        res.status = 500;
        res.set_content("Internal Server Error", "text/plain");
    });

    // 设置日志
    server_->set_logger([](const HttpRequest& req, const HttpResponse& res) {
        LOG_DEBUG("HTTP: {} {} -> {} ({})", req.method, req.path, res.status,
                  res.get_header_value("Content-Length"));
    });

    // 如果配置了静态文件目录，则设置
    if (!config_.static_dir.empty()) {
        if (!SetStaticFileDir(config_.static_mount, config_.static_dir)) {
            LOG_WARN("设置静态文件目录失败: {}", config_.static_dir);
        }
    }

    LOG_INFO("HTTP 服务器初始化完成");
    return true;
}

bool HttpServer::Start() {
    if (running_) {
        LOG_WARN("HTTP 服务器已在运行");
        return true;
    }

    if (!server_) {
        LOG_ERROR("HTTP 服务器未初始化");
        return false;
    }

    running_ = true;

    // 在独立线程中启动服务器
    server_thread_ = std::thread(&HttpServer::ServerThread, this);

    LOG_INFO("HTTP 服务器启动: {}:{}", config_.host, config_.port);
    return true;
}

void HttpServer::Stop() {
    if (!running_) {
        return;
    }

    LOG_INFO("停止 HTTP 服务器...");

    running_ = false;

    // 停止 httplib 服务器
    if (server_) {
        server_->stop();
    }

    // 等待服务器线程结束
    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    LOG_INFO("HTTP 服务器已停止");
}

bool HttpServer::IsRunning() const {
    return running_ && server_ && server_->is_running();
}

void HttpServer::Get(const std::string& pattern, HttpHandler handler) {
    if (!server_) {
        LOG_ERROR("HTTP 服务器未初始化，无法注册 GET 路由: {}", pattern);
        return;
    }
    server_->Get(pattern, std::move(handler));
    LOG_DEBUG("注册 GET 路由: {}", pattern);
}

void HttpServer::Post(const std::string& pattern, HttpHandler handler) {
    if (!server_) {
        LOG_ERROR("HTTP 服务器未初始化，无法注册 POST 路由: {}", pattern);
        return;
    }
    server_->Post(pattern, std::move(handler));
    LOG_DEBUG("注册 POST 路由: {}", pattern);
}

void HttpServer::Put(const std::string& pattern, HttpHandler handler) {
    if (!server_) {
        LOG_ERROR("HTTP 服务器未初始化，无法注册 PUT 路由: {}", pattern);
        return;
    }
    server_->Put(pattern, std::move(handler));
    LOG_DEBUG("注册 PUT 路由: {}", pattern);
}

void HttpServer::Delete(const std::string& pattern, HttpHandler handler) {
    if (!server_) {
        LOG_ERROR("HTTP 服务器未初始化，无法注册 DELETE 路由: {}", pattern);
        return;
    }
    server_->Delete(pattern, std::move(handler));
    LOG_DEBUG("注册 DELETE 路由: {}", pattern);
}

bool HttpServer::SetStaticFileDir(const std::string& mount_point,
                                   const std::string& dir) {
    if (!server_) {
        LOG_ERROR("HTTP 服务器未初始化");
        return false;
    }

    // 检查目录是否存在
    auto ret = server_->set_mount_point(mount_point, dir);
    if (!ret) {
        LOG_ERROR("设置静态文件目录失败: {} -> {}", mount_point, dir);
        return false;
    }

    LOG_INFO("设置静态文件目录: {} -> {}", mount_point, dir);
    return true;
}

std::string HttpServer::GetListenAddress() const {
    return config_.host + ":" + std::to_string(config_.port);
}

void HttpServer::ServerThread() {
    LOG_INFO("HTTP 服务器线程启动");

    // 阻塞监听
    bool success = server_->listen(config_.host, config_.port);

    if (!success && running_) {
        LOG_ERROR("HTTP 服务器监听失败: {}:{}", config_.host, config_.port);
    }

    running_ = false;
    LOG_INFO("HTTP 服务器线程退出");
}
