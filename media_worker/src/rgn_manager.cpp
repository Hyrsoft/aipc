#include "rgn_manager.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "rk_defines.h"
#include "rk_mpi_rgn.h"

namespace media_worker {
namespace {

constexpr std::size_t kRequestedBoxes = 8;
constexpr std::size_t kEdgesPerBox = 4;
constexpr RGN_HANDLE kFirstHandle = 0;
constexpr RK_U32 kColor = 0xff0000;
constexpr int kLineThickness = 4;
constexpr int kCoverThickness = 8;

std::string MpiError(const char* operation, RK_S32 result) {
    std::ostringstream message;
    message << operation << " failed: 0x" << std::hex << result;
    return message.str();
}

int ClampEven(int value, int minimum, int maximum) {
    value = std::max(minimum, std::min(value, maximum));
    return value & ~1;
}

int AlignDown(int value, int alignment) {
    return value / alignment * alignment;
}

}  // namespace

RgnManager::RgnManager(int venc_channel, int vpss_group, int vpss_channel,
                       int vi_device, int frame_width, int frame_height)
    : venc_channel_(venc_channel),
      vpss_group_(vpss_group),
      vpss_channel_(vpss_channel),
      vi_device_(vi_device),
      frame_width_(frame_width),
      frame_height_(frame_height) {}

RgnManager::~RgnManager() { Deinit(); }

nlohmann::json RgnManager::Probe(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool available = EnsureInitialized(error);
    return {{"line", available && backend_ == Backend::kLine},
            {"cover", available && backend_ == Backend::kCover},
            {"backend", available ? BackendName() : "none"},
            {"target", available ? TargetName() : "none"},
            {"max_boxes", max_boxes_},
            {"implemented", available}};
}

bool RgnManager::SetMode(const std::string& mode, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode == "embedded_rgn") {
        if (!EnsureInitialized(error)) return false;
        embedded_ = true;
        return true;
    }
    if (mode != "off" && mode != "metadata") {
        *error = "invalid OSD mode";
        return false;
    }
    embedded_ = false;
    HideAllLocked();
    return true;
}

bool RgnManager::Update(const std::vector<OsdRegion>& regions, int ttl_ms,
                        std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!embedded_) {
        *error = "embedded_rgn mode is not enabled";
        return false;
    }
    if (!EnsureInitialized(error)) return false;
    if (!ApplyLocked(regions, error)) return false;
    expires_at_ = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(std::clamp(ttl_ms, 50, 2000));
    return true;
}

void RgnManager::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    HideAllLocked();
}

bool RgnManager::EnsureInitialized(std::string* error) {
    if (initialized_) return true;
    std::string line_error;
    if (TryInitialize(Backend::kLine, AttachTarget::kVenc, &line_error)) {
        initialized_ = true;
    } else {
        // The RV1106 SDK sample only attaches COVER_RGN to VI. In practice the
        // VENC and VPSS APIs can accept COVER attributes without rendering
        // them, so use the documented VI attachment point first.
        std::string vi_cover_error;
        if (TryInitialize(Backend::kCover, AttachTarget::kVi,
                          &vi_cover_error)) {
            initialized_ = true;
        } else {
            std::string vpss_cover_error;
            if (!TryInitialize(Backend::kCover, AttachTarget::kVpssMain,
                               &vpss_cover_error)) {
                *error = "VENC LINE_RGN unavailable: " + line_error +
                         "; VI COVER_RGN unavailable: " + vi_cover_error +
                         "; main VPSS COVER_RGN unavailable: " +
                         vpss_cover_error;
                return false;
            }
            initialized_ = true;
        }
    }
    watchdog_running_.store(true);
    watchdog_ = std::thread(&RgnManager::WatchdogLoop, this);
    return true;
}

bool RgnManager::TryInitialize(Backend backend, AttachTarget target,
                               std::string* error) {
    for (RGN_HANDLE handle : handles_) {
        RK_MPI_RGN_DetachFromChn(handle, &channel_);
        RK_MPI_RGN_Destroy(handle);
    }
    handles_.clear();
    backend_ = backend;
    target_ = target;
    channel_ = ChannelForTarget(target);
    for (std::size_t index = 0; index < kRequestedBoxes * kEdgesPerBox; ++index) {
        const RGN_HANDLE handle = kFirstHandle + static_cast<RGN_HANDLE>(index);
        std::string create_error;
        if (!CreateAndAttach(handle, backend, &create_error)) {
            if (handles_.size() < kEdgesPerBox) {
                for (RGN_HANDLE created : handles_) {
                    RK_MPI_RGN_DetachFromChn(created, &channel_);
                    RK_MPI_RGN_Destroy(created);
                }
                handles_.clear();
                backend_ = Backend::kNone;
                target_ = AttachTarget::kNone;
                *error = create_error;
                return false;
            }
            break;
        }
        handles_.push_back(handle);
    }
    max_boxes_ = handles_.size() / kEdgesPerBox;
    for (std::size_t index = max_boxes_ * kEdgesPerBox; index < handles_.size();
         ++index) {
        RK_MPI_RGN_DetachFromChn(handles_[index], &channel_);
        RK_MPI_RGN_Destroy(handles_[index]);
    }
    handles_.resize(max_boxes_ * kEdgesPerBox);
    if (max_boxes_ == 0) {
        *error = "no complete RGN rectangle could be allocated";
        return false;
    }
    const OsdRegion probe{16, 16, 128, 128};
    for (int edge = 0; edge < static_cast<int>(kEdgesPerBox); ++edge) {
        if (!SetHandleLocked(static_cast<std::size_t>(edge), probe, edge, true,
                             error)) {
            for (RGN_HANDLE created : handles_) {
                RK_MPI_RGN_DetachFromChn(created, &channel_);
                RK_MPI_RGN_Destroy(created);
            }
            handles_.clear();
            max_boxes_ = 0;
            backend_ = Backend::kNone;
            target_ = AttachTarget::kNone;
            return false;
        }
    }
    HideAllLocked();
    return true;
}

bool RgnManager::CreateAndAttach(RGN_HANDLE handle, Backend backend,
                                 std::string* error) {
    RGN_ATTR_S attr{};
    attr.enType = backend == Backend::kLine ? LINE_RGN : COVER_RGN;
    RK_S32 result = RK_MPI_RGN_Create(handle, &attr);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_RGN_Create", result);
        return false;
    }
    RGN_CHN_ATTR_S display{};
    display.bShow = RK_FALSE;
    display.enType = attr.enType;
    if (backend == Backend::kLine) {
        display.unChnAttr.stLineChn.u32Thick = kLineThickness;
        display.unChnAttr.stLineChn.u32Color = kColor;
    } else {
        display.unChnAttr.stCoverChn.stRect = {0, 0, kCoverThickness,
                                               kCoverThickness};
        display.unChnAttr.stCoverChn.u32Color = kColor;
        display.unChnAttr.stCoverChn.u32Layer = handle - kFirstHandle;
        display.unChnAttr.stCoverChn.enCoordinate = RGN_ABS_COOR;
    }
    result = RK_MPI_RGN_AttachToChn(handle, &channel_, &display);
    if (result != RK_SUCCESS) {
        RK_MPI_RGN_Destroy(handle);
        *error = MpiError("RK_MPI_RGN_AttachToChn", result);
        return false;
    }
    return true;
}

bool RgnManager::ApplyLocked(const std::vector<OsdRegion>& regions,
                             std::string* error) {
    const std::size_t count = std::min(regions.size(), max_boxes_);
    for (std::size_t box = 0; box < max_boxes_; ++box) {
        for (int edge = 0; edge < static_cast<int>(kEdgesPerBox); ++edge) {
            if (!SetHandleLocked(box * kEdgesPerBox + edge,
                                 box < count ? regions[box] : OsdRegion{}, edge,
                                 box < count, error)) {
                return false;
            }
        }
    }
    return true;
}

bool RgnManager::SetHandleLocked(std::size_t index, const OsdRegion& input,
                                 int edge, bool show, std::string* error) {
    RGN_CHN_ATTR_S display{};
    display.bShow = show ? RK_TRUE : RK_FALSE;
    display.enType = backend_ == Backend::kLine ? LINE_RGN : COVER_RGN;
    const int minimum = backend_ == Backend::kCover ? kCoverThickness : 2;
    const int x = ClampEven(input.x, 0, std::max(0, frame_width_ - minimum));
    const int y = ClampEven(input.y, 0, std::max(0, frame_height_ - minimum));
    const int right = ClampEven(input.x + input.width, x + minimum, frame_width_);
    const int bottom = ClampEven(input.y + input.height, y + minimum, frame_height_);
    if (backend_ == Backend::kLine) {
        auto& line = display.unChnAttr.stLineChn;
        line.u32Thick = kLineThickness;
        line.u32Color = kColor;
        if (edge == 0) { line.stStartPoint = {x, y}; line.stEndPoint = {right, y}; }
        if (edge == 1) { line.stStartPoint = {right, y}; line.stEndPoint = {right, bottom}; }
        if (edge == 2) { line.stStartPoint = {right, bottom}; line.stEndPoint = {x, bottom}; }
        if (edge == 3) { line.stStartPoint = {x, bottom}; line.stEndPoint = {x, y}; }
    } else {
        auto& cover = display.unChnAttr.stCoverChn;
        const int width = std::max(
            kCoverThickness,
            AlignDown(std::min(right - x, frame_width_ - x), kCoverThickness));
        const int height = std::max(
            kCoverThickness,
            AlignDown(std::min(bottom - y, frame_height_ - y), kCoverThickness));
        cover.u32Color = kColor;
        cover.u32Layer = static_cast<RK_U32>(index);
        cover.enCoordinate = RGN_ABS_COOR;
        if (edge == 0)
            cover.stRect = {x, y, static_cast<RK_U32>(width), kCoverThickness};
        if (edge == 1)
            cover.stRect = {x + width - kCoverThickness, y, kCoverThickness,
                            static_cast<RK_U32>(height)};
        if (edge == 2)
            cover.stRect = {x, y + height - kCoverThickness,
                            static_cast<RK_U32>(width), kCoverThickness};
        if (edge == 3)
            cover.stRect = {x, y, kCoverThickness, static_cast<RK_U32>(height)};
    }
    const RK_S32 result =
        RK_MPI_RGN_SetDisplayAttr(handles_.at(index), &channel_, &display);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_RGN_SetDisplayAttr", result);
        return false;
    }
    return true;
}

void RgnManager::HideAllLocked() {
    if (handles_.empty()) return;
    std::string ignored;
    for (std::size_t index = 0; index < handles_.size(); ++index) {
        SetHandleLocked(index, OsdRegion{}, static_cast<int>(index % kEdgesPerBox), false,
                        &ignored);
    }
    expires_at_ = {};
}

void RgnManager::WatchdogLoop() {
    while (watchdog_running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> lock(mutex_);
        if (embedded_ && expires_at_ != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() >= expires_at_) {
            HideAllLocked();
        }
    }
}

const char* RgnManager::BackendName() const {
    if (backend_ == Backend::kLine) return "line";
    if (backend_ == Backend::kCover) return "cover";
    return "none";
}

const char* RgnManager::TargetName() const {
    if (target_ == AttachTarget::kVenc) return "venc";
    if (target_ == AttachTarget::kVpssMain) return "vpss_main";
    if (target_ == AttachTarget::kVi) return "vi";
    return "none";
}

MPP_CHN_S RgnManager::ChannelForTarget(AttachTarget target) const {
    if (target == AttachTarget::kVenc) return {RK_ID_VENC, 0, venc_channel_};
    if (target == AttachTarget::kVpssMain)
        return {RK_ID_VPSS, vpss_group_, vpss_channel_};
    if (target == AttachTarget::kVi)
        return {RK_ID_VI, vi_device_, VI_MAX_CHN_NUM};
    return {};
}

void RgnManager::Deinit() {
    watchdog_running_.store(false);
    if (watchdog_.joinable()) watchdog_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    HideAllLocked();
    for (RGN_HANDLE handle : handles_) {
        RK_MPI_RGN_DetachFromChn(handle, &channel_);
        RK_MPI_RGN_Destroy(handle);
    }
    handles_.clear();
    max_boxes_ = 0;
    backend_ = Backend::kNone;
    target_ = AttachTarget::kNone;
    initialized_ = false;
}

}  // namespace media_worker
