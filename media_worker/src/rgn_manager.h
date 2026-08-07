#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "rk_common.h"
#include "rk_comm_rgn.h"

namespace media_worker {

struct OsdRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

class RgnManager {
public:
    RgnManager(int venc_channel, int vpss_group, int vpss_channel, int vi_device,
               int frame_width, int frame_height);
    ~RgnManager();

    nlohmann::json Probe(std::string* error);
    bool SetMode(const std::string& mode, std::string* error);
    bool Update(const std::vector<OsdRegion>& regions, int ttl_ms, std::string* error);
    void Clear();
    void Deinit();

private:
    enum class Backend { kNone, kLine, kCover };
    enum class AttachTarget { kNone, kVenc, kVpssMain, kVi };

    bool EnsureInitialized(std::string* error);
    bool TryInitialize(Backend backend, AttachTarget target, std::string* error);
    bool CreateAndAttach(RGN_HANDLE handle, Backend backend, std::string* error);
    bool ApplyLocked(const std::vector<OsdRegion>& regions, std::string* error);
    bool SetHandleLocked(std::size_t index, const OsdRegion& region, int edge,
                         bool show, std::string* error);
    void HideAllLocked();
    void WatchdogLoop();
    const char* BackendName() const;
    const char* TargetName() const;
    MPP_CHN_S ChannelForTarget(AttachTarget target) const;

    int venc_channel_;
    int vpss_group_;
    int vpss_channel_;
    int vi_device_;
    int frame_width_;
    int frame_height_;
    MPP_CHN_S channel_{};
    Backend backend_ = Backend::kNone;
    AttachTarget target_ = AttachTarget::kNone;
    std::vector<RGN_HANDLE> handles_;
    std::size_t max_boxes_ = 0;
    bool embedded_ = false;
    bool initialized_ = false;
    std::chrono::steady_clock::time_point expires_at_{};
    std::mutex mutex_;
    std::atomic<bool> watchdog_running_{false};
    std::thread watchdog_;
};

}  // namespace media_worker
