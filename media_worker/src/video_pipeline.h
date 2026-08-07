#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <memory>
#include <mutex>

#include "config.h"
#include "ai_frame_ipc_publisher.h"
#include "event_emitter.h"
#include "pipeline_stats.h"
#include "rgn_manager.h"
#include "video_ipc_publisher.h"

#include "rk_common.h"

namespace media_worker {

class VideoPipeline {
public:
    using FatalCallback = std::function<void(const std::string&)>;

    VideoPipeline(const WorkerConfig& config, EventEmitter* events, FatalCallback fatal_callback);
    ~VideoPipeline();

    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    bool Init(std::string* error);
    bool Start(std::string* error);
    bool ReconfigureAiInput(const AiInputConfig& config, std::string* error);
    bool PauseAiFrames(std::string* error);
    bool ResumeAiFrames(std::string* error);
    nlohmann::json ProbeRegionCapability(std::string* error);
    bool SetOsdMode(const std::string& mode, std::string* error);
    bool UpdateRegions(const std::vector<OsdRegion>& regions, int ttl_ms,
                       std::string* error);
    void Stop();
    void Deinit();
    nlohmann::json Stats() const;

private:
    bool InitVi(std::string* error);
    bool InitVpss(std::string* error);
    bool ConfigureAiVpss(const AiInputConfig& config, std::string* error);
    void DisableAiVpss();
    bool InitVenc(std::string* error);
    bool Bind(std::string* error);
    void FetchLoop();
    void AiFetchLoop();
    AiFrameTransform ComputeAiTransform(const AiInputConfig& config) const;
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
    std::thread ai_fetch_thread_;
    std::unique_ptr<VideoIpcPublisher> ipc_publisher_;
    std::unique_ptr<AiFrameIpcPublisher> ai_ipc_publisher_;
    std::unique_ptr<RgnManager> rgn_manager_;
    std::atomic<bool> ai_capture_running_{false};
    std::atomic<bool> ai_paused_{false};
    std::atomic<bool> ai_ready_reported_{false};
    std::atomic<std::uint64_t> ai_frames_{0};
    std::atomic<std::uint64_t> ai_timeouts_{0};
    std::atomic<std::uint64_t> ai_errors_{0};
    std::uint64_t ai_sequence_ = 0;
    mutable std::mutex ai_config_mutex_;
    std::mutex ai_capture_mutex_;
    std::condition_variable ai_capture_cv_;
    bool ai_capture_active_ = false;
    std::uint64_t ipc_sequence_ = 0;
    std::FILE* output_ = nullptr;

    bool vi_device_enabled_ = false;
    bool vi_channel_enabled_ = false;
    bool vpss_group_created_ = false;
    bool vpss_channel_enabled_ = false;
    bool ai_vpss_channel_enabled_ = false;
    bool vpss_started_ = false;
    bool venc_created_ = false;
    bool venc_receiving_ = false;
    bool vi_vpss_bound_ = false;
    bool vpss_venc_bound_ = false;

    MPP_CHN_S vi_channel_{};
    MPP_CHN_S vpss_group_{};
    MPP_CHN_S vpss_channel_{};
    MPP_CHN_S venc_channel_{};
};

}  // namespace media_worker
