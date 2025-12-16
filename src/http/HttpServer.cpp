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
            // set_mount_point works for static files
            if (!server_->set_mount_point("/", doc_root)) {
                SPDLOG_ERROR("Failed to mount {} to /", doc_root);
                return false;
            }

            // Also handle the case where the user requests / without index.html
            // httplib might handle this automatically if set_mount_point is used, 
            // but let's verify or just rely on it. 
            // Actually set_mount_point serves files from that directory.
            
            running_ = true;
            server_thread_ = std::thread([this, port]() {
                SPDLOG_INFO("HTTP Server starting on port {}", port);
                // Listen on all interfaces
                server_->listen("0.0.0.0", port);
                SPDLOG_INFO("HTTP Server stopped");
            });
            
            // Detach or keep joinable? 
            // If we want to stop it cleanly in destructor, we keep it joinable.
            
            return true;
        }

        void HttpServer::stop() {
            if (running_) {
                server_->stop();
                if (server_thread_.joinable()) {
                    server_thread_.join();
                }
                running_ = false;
            }
        }

    } // namespace http
} // namespace aipc
