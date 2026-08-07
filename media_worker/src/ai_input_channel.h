#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "ai_frame_ipc_publisher.h"
#include "config.h"
#include "event_emitter.h"

namespace media_worker {

class AiInputChannel {
public:
    AiInputChannel(const VpssConfig& vpss, const VideoConfig& video,
                   const AiInputConfig& input, EventEmitter* events);
    ~AiInputChannel();

    AiInputChannel(const AiInputChannel&) = delete;
    AiInputChannel& operator=(const AiInputChannel&) = delete;

    bool ConfigureInitial(std::string* error);
    bool Start(std::string* error);
    bool Pause(std::string* error);
    bool Resume(std::string* error);
    bool Reconfigure(const AiInputConfig& next, std::string* error);
    void Stop();
    void Deinit();
    nlohmann::json Stats() const;

private:
    bool ConfigureVpss(const AiInputConfig& config, std::string* error);
    void DisableVpss();
    AiFrameTransform ComputeTransform(const AiInputConfig& config) const;
    void FetchLoop();

    VpssConfig vpss_;
    VideoConfig video_;
    AiInputConfig input_;
    EventEmitter* events_;
    std::unique_ptr<AiFrameIpcPublisher> publisher_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> ready_reported_{false};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> timeouts_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::uint64_t sequence_ = 0;
    std::thread thread_;
    mutable std::mutex config_mutex_;
    std::mutex capture_mutex_;
    std::condition_variable capture_cv_;
    bool capture_active_ = false;
    bool channel_enabled_ = false;
};

}  // namespace media_worker
