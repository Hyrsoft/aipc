#pragma once

#include <string>
#include <thread>
#include <memory>
#include <atomic>
#include <chrono>

namespace httplib {
    class Server;
}

namespace aipc {
    namespace http {

        class HttpServer {
        public:
            HttpServer();
            ~HttpServer();

            bool start(int port, const std::string &doc_root);
            void stop();

        private:
            std::unique_ptr<httplib::Server> server_;
            std::thread server_thread_;
            std::atomic<bool> running_{false};
        };

    } // namespace http
} // namespace aipc
