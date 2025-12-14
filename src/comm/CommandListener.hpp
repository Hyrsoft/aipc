#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace aipc::comm {

    struct CommandMessage {
        std::string type;      // "webrtc_offer", "webrtc_answer", "control_cmd", etc.
        std::string payload;   // JSON payload or command data
    };

    class CommandListener {
    public:
        using Handler = std::function<std::string(const CommandMessage &command)>;

        CommandListener(int port, Handler handler);
        ~CommandListener();

        CommandListener(const CommandListener &) = delete;
        CommandListener &operator=(const CommandListener &) = delete;

        bool start();
        void stop();

    private:
        void run();
        CommandMessage parse_command(const std::string &raw_data);

        // Buffer size increased to 64KB to accommodate large SDP payloads
        static constexpr size_t BUFFER_SIZE = 65536;

        int port_;
        Handler handler_;
        std::atomic<bool> running_{false};
        int sockfd_{-1};
        std::thread thread_;
    };

} // namespace aipc::comm
