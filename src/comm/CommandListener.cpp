#include "CommandListener.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <nlohmann/json.hpp>

#include <spdlog/spdlog.h>
#include "webrtc/WebRTCStreamer.hpp"

namespace aipc::comm {

    CommandListener::CommandListener(int port, Handler handler) : port_(port), handler_(std::move(handler)) {}

    CommandListener::~CommandListener() { stop(); }

    bool CommandListener::start() {
        if (running_.exchange(true)) {
            return true;
        }

        sockfd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd_ < 0) {
            SPDLOG_ERROR("CommandListener socket() failed: {}", std::strerror(errno));
            running_ = false;
            return false;
        }

        // Set socket option to allow larger UDP packets
        int bufsize = BUFFER_SIZE;
        if (::setsockopt(sockfd_, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize)) < 0) {
            SPDLOG_WARN("CommandListener setsockopt SO_RCVBUF failed: {}", std::strerror(errno));
        }

        sockaddr_in servaddr;
        std::memset(&servaddr, 0, sizeof(servaddr));
        servaddr.sin_family = AF_INET;
        servaddr.sin_addr.s_addr = INADDR_ANY;
        servaddr.sin_port = htons(static_cast<uint16_t>(port_));

        if (::bind(sockfd_, reinterpret_cast<const sockaddr *>(&servaddr), sizeof(servaddr)) < 0) {
            SPDLOG_ERROR("CommandListener bind() failed: {}", std::strerror(errno));
            ::close(sockfd_);
            sockfd_ = -1;
            running_ = false;
            return false;
        }

        thread_ = std::thread(&CommandListener::run, this);
        return true;
    }

    void CommandListener::stop() {
        if (!running_.exchange(false)) {
            return;
        }

        if (sockfd_ >= 0) {
            ::close(sockfd_);
            sockfd_ = -1;
        }

        if (thread_.joinable()) {
            thread_.join();
        }
    }

    CommandMessage CommandListener::parseCommand(const std::string &raw_data) {
        // Handle empty input
        if (raw_data.empty()) {
            SPDLOG_WARN("Received empty command data");
            return CommandMessage{"unknown", ""};
        }

        try {
            auto json = nlohmann::json::parse(raw_data);
            
            // Safely extract fields using .value() with defaults to prevent exceptions
            std::string type = json.value("type", "unknown");
            std::string payload = json.value("payload", "");
            
            // Additional validation: if payload is required for certain types
            if ((type == "webrtc_offer" || type == "webrtc_answer" || type == "webrtc_candidate" || 
                 type == "model_switch") && payload.empty()) {
                SPDLOG_WARN("Command type '{}' missing required 'payload' field", type);
            }
            
            return CommandMessage{type, payload};
            
        } catch (const nlohmann::json::parse_error &e) {
            SPDLOG_WARN("JSON parse error: {} (data: {})", e.what(), raw_data.substr(0, 100));
            // Fallback: treat entire message as simple command (legacy support)
            return CommandMessage{"unknown", raw_data};
        } catch (const std::exception &e) {
            SPDLOG_ERROR("Unexpected error parsing command: {}", e.what());
            return CommandMessage{"unknown", raw_data};
        }
    }

    void CommandListener::run() {
        SPDLOG_INFO("CommandListener started on port {}", port_);

        auto buffer = std::make_unique<char[]>(BUFFER_SIZE);

        while (running_) {
            sockaddr_in cliaddr;
            socklen_t len = sizeof(cliaddr);

            const int n = ::recvfrom(sockfd_, buffer.get(), BUFFER_SIZE - 1, 0, reinterpret_cast<sockaddr *>(&cliaddr),
                                     &len);
            if (n < 0) {
                if (!running_) {
                    break;
                }
                SPDLOG_WARN("CommandListener recvfrom error: {}", std::strerror(errno));
                continue;
            }
            
            // [修复1]：处理空接收（可能由于网络问题）
            if (n == 0) {
                SPDLOG_DEBUG("Received empty packet");
                continue;
            }

            // Ensure null termination
            buffer[n] = '\0';
            const std::string raw_data(buffer.get(), n);

            SPDLOG_DEBUG("Received raw data (size: {}): {}", raw_data.size(), raw_data.substr(0, 100));

            // Parse the command
            CommandMessage cmd = parseCommand(raw_data);
            
            // [修复2]：跳过 'unknown' 类型且 payload 为空的命令，避免不必要的处理
            if (cmd.type == "unknown" && cmd.payload.empty()) {
                SPDLOG_DEBUG("Skipping empty/malformed command");
                continue;
            }

            if (handler_) {
                std::string response;
                try {
                    response = handler_(cmd);
                } catch (const std::exception &e) {
                    SPDLOG_ERROR("Handler exception: {}", e.what());
                    // Send error response
                    nlohmann::json error_resp;
                    error_resp["type"] = "error";
                    error_resp["message"] = std::string("Handler error: ") + e.what();
                    response = error_resp.dump();
                }

                // Send response back to client (handle empty responses gracefully)
                if (!response.empty()) {
                    int sent = ::sendto(sockfd_, response.c_str(), response.size(), 0, 
                                       reinterpret_cast<const sockaddr *>(&cliaddr), len);
                    if (sent < 0) {
                        SPDLOG_WARN("sendto failed: {}", std::strerror(errno));
                    } else {
                        SPDLOG_DEBUG("Sent response (size: {}) back to client", sent);
                    }
                }
            }
        }
        
        SPDLOG_INFO("CommandListener stopped");
    }

} // namespace aipc::comm
