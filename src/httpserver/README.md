# HTTP Server 模块

基于 [cpp-httplib](https://github.com/yhirose/cpp-httplib) 的 HTTP 服务器封装模块。

## 功能特性

- 简单易用的 HTTP 服务器接口
- 支持 GET/POST/PUT/DELETE 请求
- 支持静态文件服务
- 非阻塞启动（在独立线程中运行）
- 线程安全

## 使用方法

### 基本使用

```cpp
#include "httpserver/http_server.h"

// 创建服务器
HttpServer server;

// 配置
HttpServerConfig config;
config.host = "0.0.0.0";
config.port = 8080;
config.static_dir = "/app/www";  // 静态文件目录

// 初始化
server.Init(config);

// 注册 API 路由
server.Get("/api/status", [](const HttpRequest& req, HttpResponse& res) {
    res.set_content("{\"status\": \"ok\"}", "application/json");
});

server.Post("/api/data", [](const HttpRequest& req, HttpResponse& res) {
    // 处理 POST 请求
    auto body = req.body;
    res.set_content("{\"received\": true}", "application/json");
});

// 启动服务器（非阻塞）
server.Start();

// ... 其他业务逻辑 ...

// 停止服务器
server.Stop();
```

### 静态文件服务

```cpp
// 方式1：在配置中指定
HttpServerConfig config;
config.static_dir = "/app/www";
config.static_mount = "/";  // 挂载点
server.Init(config);

// 方式2：手动设置
server.SetStaticFileDir("/static", "/app/static");
```

### 路由参数

```cpp
// 支持路径参数（使用正则表达式）
server.Get(R"(/api/user/(\d+))", [](const HttpRequest& req, HttpResponse& res) {
    auto user_id = req.matches[1];
    res.set_content("User ID: " + std::string(user_id), "text/plain");
});

// 查询参数
server.Get("/api/search", [](const HttpRequest& req, HttpResponse& res) {
    auto keyword = req.get_param_value("q");
    res.set_content("Search: " + keyword, "text/plain");
});
```

## API 参考

### HttpServerConfig

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| host | string | "0.0.0.0" | 监听地址 |
| port | int | 8080 | 监听端口 |
| static_dir | string | "" | 静态文件目录 |
| static_mount | string | "/" | 静态文件挂载路径 |
| thread_pool_size | int | 4 | 线程池大小 |

### HttpServer 方法

| 方法 | 说明 |
|------|------|
| Init(config) | 初始化服务器 |
| Start() | 启动服务器（非阻塞） |
| Stop() | 停止服务器 |
| IsRunning() | 检查运行状态 |
| Get(pattern, handler) | 注册 GET 路由 |
| Post(pattern, handler) | 注册 POST 路由 |
| Put(pattern, handler) | 注册 PUT 路由 |
| Delete(pattern, handler) | 注册 DELETE 路由 |
| SetStaticFileDir(mount, dir) | 设置静态文件目录 |

## 依赖

- cpp-httplib (thirdparty/cpp-httplib)
- spdlog (日志)
- pthread (线程)

## 注意事项

1. cpp-httplib 是 header-only 库，但需要链接 pthread
2. 如果需要 HTTPS 支持，需要链接 OpenSSL
3. 服务器在独立线程中运行，Start() 调用立即返回
