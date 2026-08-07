#include "ai_input_channel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <utility>

#include "rk_mpi_mb.h"
#include "rk_mpi_vpss.h"

namespace media_worker {
namespace {

std::string MpiError(const char* operation, RK_S32 result) {
    std::ostringstream message;
    message << operation << " failed: 0x" << std::hex << result;
    return message.str();
}

}  // namespace

AiInputChannel::AiInputChannel(const VpssConfig& vpss, const VideoConfig& video,
                               const AiInputConfig& input, EventEmitter* events)
    : vpss_(vpss), video_(video), input_(input), events_(events) {
    if (input_.ipc_fd >= 0) {
        publisher_ = std::make_unique<AiFrameIpcPublisher>(
            input_.ipc_fd, [this](const std::string& message) {
                events_->Emit("Warning", {{"media", "ai_input"},
                                           {"reason", "ipc_disabled"},
                                           {"message", message}});
            });
    }
}

AiInputChannel::~AiInputChannel() {
    Deinit();
}

bool AiInputChannel::ConfigureInitial(std::string* error) {
    return !input_.enabled || ConfigureVpss(input_, error);
}

bool AiInputChannel::ConfigureVpss(const AiInputConfig& config, std::string* error) {
    const auto transform = ComputeTransform(config);
    VPSS_CHN_ATTR_S attr{};
    attr.enChnMode = VPSS_CHN_MODE_USER;
    attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
    attr.enPixelFormat = RK_FMT_YUV420SP;
    attr.stFrameRate.s32SrcFrameRate = video_.fps;
    attr.stFrameRate.s32DstFrameRate = config.fps;
    attr.u32Width = static_cast<RK_U32>(
        config.fit_mode == "contain"
            ? config.width - transform.pad_left - transform.pad_right
            : config.width);
    attr.u32Height = static_cast<RK_U32>(
        config.fit_mode == "contain"
            ? config.height - transform.pad_top - transform.pad_bottom
            : config.height);
    attr.u32Depth = static_cast<RK_U32>(config.depth);
    attr.u32FrameBufCnt = static_cast<RK_U32>(config.buffer_count);
    attr.enCompressMode = COMPRESS_MODE_NONE;
    attr.stAspectRatio.enMode = ASPECT_RATIO_NONE;
    attr.stAspectRatio.u32BgColor = 0;
    RK_S32 result = RK_MPI_VPSS_SetChnAttr(vpss_.group_id, config.channel_id, &attr);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_VPSS_SetChnAttr(ai)", result);
        return false;
    }
    VPSS_CROP_INFO_S crop{};
    if (config.fit_mode == "cover") {
        crop.bEnable = RK_TRUE;
        crop.enCropCoordinate = VPSS_CROP_ABS_COOR;
        crop.stCropRect.s32X = transform.crop_x;
        crop.stCropRect.s32Y = transform.crop_y;
        crop.stCropRect.u32Width = static_cast<RK_U32>(transform.crop_width);
        crop.stCropRect.u32Height = static_cast<RK_U32>(transform.crop_height);
    } else {
        crop.bEnable = RK_FALSE;
    }
    result = RK_MPI_VPSS_SetChnCrop(vpss_.group_id, config.channel_id, &crop);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_VPSS_SetChnCrop(ai)", result);
        return false;
    }
    result = RK_MPI_VPSS_EnableChn(vpss_.group_id, config.channel_id);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_VPSS_EnableChn(ai)", result);
        return false;
    }
    channel_enabled_ = true;
    events_->Emit("BootProgress", {{"stage", "ai_vpss_ready"},
                                    {"channel_id", config.channel_id},
                                    {"width", config.width},
                                    {"height", config.height},
                                    {"fps", config.fps},
                                    {"fit_mode", config.fit_mode}});
    return true;
}

void AiInputChannel::DisableVpss() {
    if (!channel_enabled_) return;
    RK_MPI_VPSS_DisableChn(vpss_.group_id, input_.channel_id);
    channel_enabled_ = false;
}

bool AiInputChannel::Start(std::string* error) {
    if (!publisher_) return true;
    if (!publisher_->Start()) {
        events_->Emit("Warning", {{"media", "ai_input"}, {"reason", "ipc_start_failed"}});
    }
    if (running_.exchange(true)) {
        *error = "AI input fetch loop already running";
        return false;
    }
    paused_.store(!input_.enabled);
    try {
        thread_ = std::thread(&AiInputChannel::FetchLoop, this);
    } catch (const std::exception& exception) {
        running_.store(false);
        *error = std::string("cannot start AI input fetch thread: ") + exception.what();
        return false;
    }
    return true;
}

AiFrameTransform AiInputChannel::ComputeTransform(const AiInputConfig& config) const {
    AiFrameTransform transform;
    transform.crop_width = video_.width;
    transform.crop_height = video_.height;
    if (config.fit_mode == "contain") {
        const double scale = std::min(static_cast<double>(config.width) / video_.width,
                                      static_cast<double>(config.height) / video_.height);
        const int content_width =
            std::max(4, static_cast<int>(std::floor(video_.width * scale)) & ~3);
        const int content_height =
            std::max(4, static_cast<int>(std::floor(video_.height * scale)) & ~3);
        transform.pad_left = ((config.width - content_width) / 2) & ~1;
        transform.pad_top = ((config.height - content_height) / 2) & ~1;
        transform.pad_right = config.width - content_width - transform.pad_left;
        transform.pad_bottom = config.height - content_height - transform.pad_top;
    } else if (config.fit_mode == "cover") {
        const double source_ratio = static_cast<double>(video_.width) / video_.height;
        const double target_ratio = static_cast<double>(config.width) / config.height;
        if (source_ratio > target_ratio) {
            transform.crop_width = static_cast<int>(video_.height * target_ratio) & ~1;
            transform.crop_x = (video_.width - transform.crop_width) / 2;
        } else if (source_ratio < target_ratio) {
            transform.crop_height = static_cast<int>(video_.width / target_ratio) & ~1;
            transform.crop_y = (video_.height - transform.crop_height) / 2;
        }
    }
    return transform;
}

void AiInputChannel::FetchLoop() {
    while (running_.load()) {
        if (paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        AiInputConfig input;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            input = input_;
        }
        {
            std::lock_guard<std::mutex> lock(capture_mutex_);
            if (paused_.load()) continue;
            capture_active_ = true;
        }
        VIDEO_FRAME_INFO_S frame{};
        RK_S32 result = RK_MPI_VPSS_GetChnFrame(vpss_.group_id, input.channel_id,
                                                &frame, 200);
        if (result != RK_SUCCESS) {
            {
                std::lock_guard<std::mutex> lock(capture_mutex_);
                capture_active_ = false;
            }
            capture_cv_.notify_all();
            if (running_.load() && !paused_.load()) timeouts_.fetch_add(1);
            continue;
        }
        const auto width = frame.stVFrame.u32Width;
        const auto height = frame.stVFrame.u32Height;
        const auto y_stride = frame.stVFrame.u32VirWidth == 0
                                  ? width
                                  : frame.stVFrame.u32VirWidth;
        const auto vir_height = frame.stVFrame.u32VirHeight == 0
                                    ? height
                                    : frame.stVFrame.u32VirHeight;
        const std::size_t expected = static_cast<std::size_t>(y_stride) * vir_height * 3 / 2;
        const std::size_t available =
            static_cast<std::size_t>(RK_MPI_MB_GetSize(frame.stVFrame.pMbBlk));
        void* address = RK_MPI_MB_Handle2VirAddr(frame.stVFrame.pMbBlk);
        RawAiFrame output;
        if (address != nullptr && expected > 0 && available >= expected) {
            const auto* bytes = static_cast<const std::uint8_t*>(address);
            const auto transform = ComputeTransform(input);
            if (input.fit_mode == "contain") {
                const std::size_t output_y = static_cast<std::size_t>(input.width) * input.height;
                output.data.assign(output_y + output_y / 2, 128);
                std::fill(output.data.begin(), output.data.begin() + output_y, 16);
                const int content_width = input.width - transform.pad_left - transform.pad_right;
                const int content_height = input.height - transform.pad_top - transform.pad_bottom;
                const auto* source_y = bytes;
                const auto* source_uv = bytes + static_cast<std::size_t>(y_stride) * vir_height;
                auto* target_y = output.data.data() +
                                 static_cast<std::size_t>(transform.pad_top) * input.width +
                                 transform.pad_left;
                auto* target_uv = output.data.data() + output_y +
                                  static_cast<std::size_t>(transform.pad_top / 2) * input.width +
                                  transform.pad_left;
                for (int row = 0; row < content_height; ++row) {
                    std::memcpy(target_y + static_cast<std::size_t>(row) * input.width,
                                source_y + static_cast<std::size_t>(row) * y_stride,
                                content_width);
                }
                for (int row = 0; row < content_height / 2; ++row) {
                    std::memcpy(target_uv + static_cast<std::size_t>(row) * input.width,
                                source_uv + static_cast<std::size_t>(row) * y_stride,
                                content_width);
                }
                output.width = input.width;
                output.height = input.height;
                output.y_stride = input.width;
                output.uv_stride = input.width;
                output.height_stride = input.height;
            } else {
                output.data.assign(bytes, bytes + expected);
                output.width = width;
                output.height = height;
                output.y_stride = y_stride;
                output.uv_stride = y_stride;
                output.height_stride = vir_height;
            }
            output.pts = frame.stVFrame.u64PTS;
            output.sequence = ++sequence_;
            output.main_width = video_.width;
            output.main_height = video_.height;
            output.fit_mode = ParseAiFitMode(input.fit_mode);
            output.transform = transform;
        } else {
            errors_.fetch_add(1);
        }
        result = RK_MPI_VPSS_ReleaseChnFrame(vpss_.group_id, input.channel_id, &frame);
        {
            std::lock_guard<std::mutex> lock(capture_mutex_);
            capture_active_ = false;
        }
        capture_cv_.notify_all();
        if (result != RK_SUCCESS) {
            errors_.fetch_add(1);
            events_->Emit("Warning", {{"media", "ai_input"},
                                       {"reason", "release_failed"},
                                       {"code", result}});
        }
        if (!output.data.empty() && publisher_) {
            const auto output_width = output.width;
            const auto output_height = output.height;
            publisher_->Enqueue(std::move(output));
            frames_.fetch_add(1);
            if (!ready_reported_.exchange(true)) {
                capture_cv_.notify_all();
                events_->Emit("AiInputReady", {{"channel_id", input.channel_id},
                                                {"width", output_width},
                                                {"height", output_height},
                                                {"fps", input.fps},
                                                {"format", "nv12"}});
            }
        }
    }
}

bool AiInputChannel::Pause(std::string* error) {
    paused_.store(true);
    std::unique_lock<std::mutex> lock(capture_mutex_);
    if (!capture_cv_.wait_for(lock, std::chrono::milliseconds(500),
                              [this] { return !capture_active_; })) {
        *error = "timed out waiting for AI VPSS frame release";
        return false;
    }
    return true;
}

bool AiInputChannel::Resume(std::string* error) {
    if (!input_.enabled || !channel_enabled_) {
        *error = "AI VPSS channel is not enabled";
        return false;
    }
    ready_reported_.store(false);
    paused_.store(false);
    std::unique_lock<std::mutex> lock(capture_mutex_);
    if (!capture_cv_.wait_for(lock, std::chrono::milliseconds(1500),
                              [this] { return ready_reported_.load(); })) {
        paused_.store(true);
        *error = "timed out waiting for the reconfigured AI input frame";
        return false;
    }
    return true;
}

bool AiInputChannel::Reconfigure(const AiInputConfig& next, std::string* error) {
    AiInputConfig previous;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        previous = input_;
    }
    if (!Pause(error)) return false;
    DisableVpss();
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        input_ = next;
    }
    if (!next.enabled || ConfigureVpss(next, error)) {
        ready_reported_.store(false);
        paused_.store(true);
        return true;
    }
    const std::string candidate_error = *error;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        input_ = previous;
    }
    if (previous.enabled) {
        std::string rollback_error;
        if (!ConfigureVpss(previous, &rollback_error)) {
            *error = candidate_error + "; rollback failed: " + rollback_error;
            return false;
        }
        paused_.store(true);
    }
    *error = candidate_error;
    return false;
}

void AiInputChannel::Stop() {
    running_.store(false);
    paused_.store(false);
    if (thread_.joinable()) thread_.join();
    if (publisher_) publisher_->Stop();
}

void AiInputChannel::Deinit() {
    Stop();
    DisableVpss();
}

nlohmann::json AiInputChannel::Stats() const {
    return {{"frames", frames_.load()},
            {"timeouts", timeouts_.load()},
            {"errors", errors_.load()},
            {"ipc_frames", publisher_ ? publisher_->Frames() : 0},
            {"ipc_bytes", publisher_ ? publisher_->Bytes() : 0},
            {"ipc_drops", publisher_ ? publisher_->Drops() : 0},
            {"ipc_errors", publisher_ ? publisher_->Errors() : 0}};
}

}  // namespace media_worker
