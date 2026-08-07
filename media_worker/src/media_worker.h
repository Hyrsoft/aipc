#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "audio_pipeline.h"
#include "config.h"
#include "event_emitter.h"
#include "metrics_sampler.h"
#include "media_control.h"
#include "media_control_service.h"
#include "media_runtime.h"
#include "video_pipeline.h"

namespace media_worker {

enum class ExitCode : int {
    kSuccess = 0,
    kConfigError = 2,
    kInitializationError = 3,
    kRuntimeError = 4,
};

class MediaWorker {
public:
    explicit MediaWorker(WorkerConfig config);
    ~MediaWorker();

    int Run(const std::function<bool()>& external_stop_requested);

private:
    bool PreflightOutputs(std::string* error);
    void RequestFatalStop(const std::string& message);
    std::string RuntimeError() const;
    void EmitMetrics(double elapsed_seconds);
    void Shutdown();

    WorkerConfig config_;
    EventEmitter events_;
    std::unique_ptr<VideoPipeline> video_;
    std::unique_ptr<AudioPipeline> audio_;
    std::unique_ptr<MediaControl> control_;
    std::unique_ptr<MediaControlService> control_service_;
    std::unique_ptr<MediaRuntime> runtime_;
    std::atomic<bool> stop_requested_{false};
    mutable std::mutex error_mutex_;
    std::string runtime_error_;
    MetricsSampler video_metrics_sampler_;
    MetricsSampler audio_metrics_sampler_;
};

}  // namespace media_worker
