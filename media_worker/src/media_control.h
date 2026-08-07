#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

#include <nlohmann/json.hpp>

namespace media_worker {

class MediaControl {
public:
    using Handler = std::function<nlohmann::json(const nlohmann::json&)>;

    MediaControl(int fd, Handler handler);
    ~MediaControl();

    bool Start();
    void Stop();

private:
    void Run();
    bool WriteMessage(const nlohmann::json& message);

    int fd_;
    Handler handler_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace media_worker
