#include "HttpServer.hpp"
#include "httplib.h"
#include <spdlog/spdlog.h>

namespace aipc {
    namespace http {

        HttpServer::HttpServer() {
            server_ = std::make_unique<httplib::Server>();
        }

        HttpServer::~HttpServer() {
            stop();
        }

        bool HttpServer::start(int port, const std::string &doc_root) {
            if (running_) {
                return false;
            }

            // Mount the document root to the root path
            if (!server_->set_mount_point("/", doc_root)) {
                SPDLOG_ERROR("Failed to mount {} to /", doc_root);
                return false;
            }

            running_ = true;
            server_thread_ = std::thread([this, port]() {
                SPDLOG_INFO("[HTTP Server] Starting on port {}", port);
                // Listen on all interfaces (blocking call)
                if (server_->listen("0.0.0.0", port)) {
                    SPDLOG_INFO("[HTTP Server] Successfully listening on port {}", port);
                } else {
                    SPDLOG_ERROR("[HTTP Server] Failed to listen on port {}", port);
                }
            });

            // Give the server thread time to start listening
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            return true;
        }

        void HttpServer::stop() {
            if (running_) {
                SPDLOG_INFO("[HTTP Server] Stopping...");
                server_->stop();
                if (server_thread_.joinable()) {
                    server_thread_.join();
                }
                running_ = false;
                SPDLOG_INFO("[HTTP Server] Stopped");
            }
        }

    } // namespace http
} // namespace aipc
