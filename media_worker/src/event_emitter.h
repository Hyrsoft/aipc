#pragma once

#include <chrono>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace media_worker {

class EventEmitter {
public:
    explicit EventEmitter(std::string generation);
    ~EventEmitter();

    EventEmitter(const EventEmitter&) = delete;
    EventEmitter& operator=(const EventEmitter&) = delete;

    void Emit(const std::string& event, nlohmann::json fields = {});

private:
    std::string generation_;
    std::chrono::steady_clock::time_point start_time_;
    std::mutex mutex_;
    int output_fd_ = -1;
};

}  // namespace media_worker
