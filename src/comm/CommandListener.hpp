#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace aipc::comm {

    class CommandListener {
    public:
        using Handler = std::function<void(const std::string &command)>;

        CommandListener(int port, Handler handler);
        ~CommandListener();

        CommandListener(const CommandListener &) = delete;
        CommandListener &operator=(const CommandListener &) = delete;

        bool start();
        void stop();

    private:
        void run();

        int port_;
        Handler handler_;
        std::atomic<bool> running_{false};
        int sockfd_{-1};
        std::thread thread_;
    };

} // namespace aipc::comm
