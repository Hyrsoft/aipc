#pragma once

#include <atomic>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

#include "config.h"
#include "event_emitter.h"
#include "pipeline_stats.h"

#include "rk_common.h"

namespace media_worker {

class AudioPipeline {
public:
    using FatalCallback = std::function<void(const std::string&)>;

    AudioPipeline(const WorkerConfig& config, EventEmitter* events, FatalCallback fatal_callback);
    ~AudioPipeline();

    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    bool Init(std::string* error);
    bool Start(std::string* error);
    void Stop();
    void Deinit();
    nlohmann::json Stats() const { return stats_.Snapshot(); }

private:
    void FetchLoop();
    void ReportTimeout(std::uint64_t consecutive_timeouts);
    void ReportFatal(const std::string& message);

    WorkerConfig config_;
    EventEmitter* events_;
    FatalCallback fatal_callback_;
    PipelineStats stats_;
    std::atomic<bool> running_{false};
    std::atomic<bool> fatal_reported_{false};
    std::atomic<bool> ready_reported_{false};
    std::thread fetch_thread_;
    std::FILE* output_ = nullptr;

    bool ai_enabled_ = false;
    bool ai_channel_enabled_ = false;
    bool ai_resample_enabled_ = false;
    bool aenc_created_ = false;
    bool ai_aenc_bound_ = false;
    MPP_CHN_S ai_channel_{};
    MPP_CHN_S aenc_channel_{};
};

}  // namespace media_worker

