#pragma once

#include <atomic>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include <memory>
#include <thread>

#include "ai_input_channel.h"
#include "config.h"
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
    bool InitVenc(std::string* error);
    bool Bind(std::string* error);
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
    std::unique_ptr<VideoIpcPublisher> ipc_publisher_;
    std::unique_ptr<AiInputChannel> ai_input_;
    std::unique_ptr<RgnManager> rgn_manager_;
    std::uint64_t ipc_sequence_ = 0;
    std::FILE* output_ = nullptr;

    bool vi_device_enabled_ = false;
    bool vi_channel_enabled_ = false;
    bool vpss_group_created_ = false;
    bool vpss_channel_enabled_ = false;
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
