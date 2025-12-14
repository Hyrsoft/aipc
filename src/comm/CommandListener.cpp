#include "CommandListener.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

namespace aipc::comm {

    CommandListener::CommandListener(int port, Handler handler) : port_(port), handler_(std::move(handler)) {}

    CommandListener::~CommandListener() { stop(); }

    bool CommandListener::start() {
        if (running_.exchange(true)) {
            return true;
        }

        sockfd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd_ < 0) {
            std::cerr << "[aipc] CommandListener socket() failed: " << std::strerror(errno) << std::endl;
            running_ = false;
            return false;
        }

        sockaddr_in servaddr;
        std::memset(&servaddr, 0, sizeof(servaddr));
        servaddr.sin_family = AF_INET;
        servaddr.sin_addr.s_addr = INADDR_ANY;
        servaddr.sin_port = htons(static_cast<uint16_t>(port_));

        if (::bind(sockfd_, reinterpret_cast<const sockaddr *>(&servaddr), sizeof(servaddr)) < 0) {
            std::cerr << "[aipc] CommandListener bind() failed: " << std::strerror(errno) << std::endl;
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

    void CommandListener::run() {
        std::cout << "[aipc] CommandListener started on port " << port_ << std::endl;

        while (running_) {
            char buffer[1024];
            sockaddr_in cliaddr;
            socklen_t len = sizeof(cliaddr);

            const int n =
                    ::recvfrom(sockfd_, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr *>(&cliaddr), &len);
            if (n <= 0) {
                if (!running_) {
                    break;
                }
                // socket may have been closed or interrupted
                continue;
            }

            buffer[n] = '\0';
            const std::string cmd(buffer);

            if (handler_) {
                handler_(cmd);
            }
        }
    }

} // namespace aipc::comm
