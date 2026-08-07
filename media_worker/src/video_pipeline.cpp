#include "video_pipeline.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <iostream>
#include <sstream>
#include <utility>
#include <vector>

#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_vpss.h"

namespace media_worker {
namespace {

std::string MpiError(const char* operation, RK_S32 result) {
    std::ostringstream message;
    message << operation << " failed: 0x" << std::hex << result;
    return message.str();
}

}  // namespace

VideoPipeline::VideoPipeline(const WorkerConfig& config, EventEmitter* events,
                             FatalCallback fatal_callback)
    : config_(config), events_(events), fatal_callback_(std::move(fatal_callback)) {}

VideoPipeline::~VideoPipeline() {
    Deinit();
}

bool VideoPipeline::Init(std::string* error) {
    if (!config_.video.output_path.empty()) {
        output_ = std::fopen(config_.video.output_path.c_str(), "wb");
        if (output_ == nullptr) {
            *error = "cannot open video debug output " + config_.video.output_path + ": " +
                     std::strerror(errno);
            return false;
        }
    }
    ai_input_ = std::make_unique<AiInputChannel>(config_.vpss, config_.video,
                                                 config_.ai_input, events_);
    if (!InitVi(error) || !InitVpss(error) || !InitVenc(error) || !Bind(error)) {
        return false;
    }
    if (config_.video.ipc_fd >= 0) {
        ipc_publisher_ = std::make_unique<VideoIpcPublisher>(
            config_.video.ipc_fd, 8, [this](const std::string& message) {
                events_->Emit("Warning", {{"media", "video"}, {"reason", "ipc_disabled"},
                                           {"message", message}});
            });
    }
    return true;
}

bool VideoPipeline::InitVi(std::string* error) {
    VI_DEV_ATTR_S device_attr{};
    RK_S32 result = RK_MPI_VI_GetDevAttr(config_.vi.device_id, &device_attr);
    if (result == RK_ERR_VI_NOT_CONFIG) {
        result = RK_MPI_VI_SetDevAttr(config_.vi.device_id, &device_attr);
        if (result != RK_SUCCESS) {
            *error = MpiError("RK_MPI_VI_SetDevAttr", result);
            return false;
        }
    } else if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_VI_GetDevAttr", result);
        return false;
    }

    result = RK_MPI_VI_GetDevIsEnable(config_.vi.device_id);
    if (result != RK_SUCCESS) {
        result = RK_MPI_VI_EnableDev(config_.vi.device_id);
        if (result != RK_SUCCESS) {
            *error = MpiError("RK_MPI_VI_EnableDev", result);
            return false;
        }
        vi_device_enabled_ = true;

        VI_DEV_BIND_PIPE_S binding{};
        binding.u32Num = 1;
        binding.PipeId[0] = config_.vi.pipe_id;
        result = RK_MPI_VI_SetDevBindPipe(config_.vi.device_id, &binding);
        if (result != RK_SUCCESS) {
            *error = MpiError("RK_MPI_VI_SetDevBindPipe", result);
            return false;
        }
    }

    VI_CHN_ATTR_S channel_attr{};
    channel_attr.stIspOpt.stMaxSize.u32Width = config_.video.width;
    channel_attr.stIspOpt.stMaxSize.u32Height = config_.video.height;
    channel_attr.stIspOpt.u32BufCount = config_.vi.buffer_count;
    channel_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    channel_attr.stSize.u32Width = config_.video.width;
    channel_attr.stSize.u32Height = config_.video.height;
    channel_attr.enPixelFormat = RK_FMT_YUV420SP;
    channel_attr.enCompressMode = COMPRESS_MODE_NONE;
    channel_attr.u32Depth = 0;
    result = RK_MPI_VI_SetChnAttr(config_.vi.device_id, config_.vi.channel_id,
                                  &channel_attr);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_VI_SetChnAttr", result);
        return false;
    }
    result = RK_MPI_VI_EnableChn(config_.vi.device_id, config_.vi.channel_id);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_VI_EnableChn", result);
        return false;
    }
    vi_channel_enabled_ = true;
    events_->Emit("BootProgress", {{"stage", "vi_ready"}});
    return true;
}

bool VideoPipeline::InitVpss(std::string* error) {
    VPSS_GRP_ATTR_S group_attr{};
    group_attr.u32MaxW = config_.video.width;
    group_attr.u32MaxH = config_.video.height;
    group_attr.enPixelFormat = RK_FMT_YUV420SP;
    group_attr.stFrameRate.s32SrcFrameRate = -1;
    group_attr.stFrameRate.s32DstFrameRate = -1;
    group_attr.enCompressMode = COMPRESS_MODE_NONE;
    RK_S32 result = RK_MPI_VPSS_CreateGrp(config_.vpss.group_id, &group_attr);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_VPSS_CreateGrp", result);
        return false;
    }
    vpss_group_created_ = true;

    VPSS_CHN_ATTR_S channel_attr{};
    channel_attr.enChnMode = VPSS_CHN_MODE_USER;
    channel_attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
    channel_attr.enPixelFormat = RK_FMT_YUV420SP;
    channel_attr.stFrameRate.s32SrcFrameRate = -1;
    channel_attr.stFrameRate.s32DstFrameRate = -1;
    channel_attr.u32Width = config_.video.width;
    channel_attr.u32Height = config_.video.height;
    channel_attr.u32Depth = 0;
    channel_attr.enCompressMode = COMPRESS_MODE_NONE;
    result = RK_MPI_VPSS_SetChnAttr(config_.vpss.group_id, config_.vpss.channel_id,
                                    &channel_attr);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_VPSS_SetChnAttr", result);
        return false;
    }
    result = RK_MPI_VPSS_EnableChn(config_.vpss.group_id, config_.vpss.channel_id);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_VPSS_EnableChn", result);
        return false;
    }
    vpss_channel_enabled_ = true;
    if (!ai_input_->ConfigureInitial(error)) {
        return false;
    }
    result = RK_MPI_VPSS_StartGrp(config_.vpss.group_id);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_VPSS_StartGrp", result);
        return false;
    }
    vpss_started_ = true;
    events_->Emit("BootProgress", {{"stage", "vpss_ready"}});
    return true;
}

bool VideoPipeline::InitVenc(std::string* error) {
    VENC_CHN_ATTR_S attr{};
    attr.stVencAttr.enType = RK_VIDEO_ID_AVC;
    attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    attr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
    attr.stVencAttr.u32PicWidth = config_.video.width;
    attr.stVencAttr.u32PicHeight = config_.video.height;
    attr.stVencAttr.u32VirWidth = config_.video.width;
    attr.stVencAttr.u32VirHeight = config_.video.height;
    attr.stVencAttr.u32StreamBufCnt = config_.video.stream_buffer_count;
    attr.stVencAttr.u32BufSize = config_.video.width * config_.video.height / 2;
    attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    attr.stRcAttr.stH264Cbr.u32Gop = config_.video.gop;
    attr.stRcAttr.stH264Cbr.u32BitRate = config_.video.bitrate_kbps;
    attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
    attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = config_.video.fps;
    attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
    attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = config_.video.fps;

    RK_S32 result = RK_MPI_VENC_CreateChn(config_.video.venc_channel_id, &attr);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_VENC_CreateChn", result);
        return false;
    }
    venc_created_ = true;
    rgn_manager_ = std::make_unique<RgnManager>(
        config_.video.venc_channel_id, config_.vpss.group_id,
        config_.vpss.channel_id, config_.vi.device_id, config_.video.width,
        config_.video.height);
    VENC_RECV_PIC_PARAM_S receive{};
    receive.s32RecvPicNum = -1;
    result = RK_MPI_VENC_StartRecvFrame(config_.video.venc_channel_id, &receive);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_VENC_StartRecvFrame", result);
        return false;
    }
    venc_receiving_ = true;
    events_->Emit("BootProgress", {{"stage", "venc_ready"}});
    return true;
}

bool VideoPipeline::Bind(std::string* error) {
    vi_channel_ = {RK_ID_VI, config_.vi.device_id, config_.vi.channel_id};
    vpss_group_ = {RK_ID_VPSS, config_.vpss.group_id, 0};
    vpss_channel_ = {RK_ID_VPSS, config_.vpss.group_id, config_.vpss.channel_id};
    venc_channel_ = {RK_ID_VENC, 0, config_.video.venc_channel_id};

    RK_S32 result = RK_MPI_SYS_Bind(&vi_channel_, &vpss_group_);
    if (result != RK_SUCCESS) {
        *error = MpiError("bind VI to VPSS", result);
        return false;
    }
    vi_vpss_bound_ = true;
    result = RK_MPI_SYS_Bind(&vpss_channel_, &venc_channel_);
    if (result != RK_SUCCESS) {
        *error = MpiError("bind VPSS to VENC", result);
        return false;
    }
    vpss_venc_bound_ = true;
    events_->Emit("BootProgress", {{"stage", "video_bound"}});
    return true;
}

bool VideoPipeline::Start(std::string* error) {
    if (running_.exchange(true)) {
        *error = "video fetch loop already running";
        return false;
    }
    if (ipc_publisher_ && !ipc_publisher_->Start()) {
        events_->Emit("Warning", {{"media", "video"}, {"reason", "ipc_start_failed"}});
    }
    try {
        fetch_thread_ = std::thread(&VideoPipeline::FetchLoop, this);
    } catch (const std::exception& exception) {
        running_.store(false);
        *error = std::string("cannot start video fetch thread: ") + exception.what();
        return false;
    }
    return ai_input_->Start(error);
}

bool VideoPipeline::PauseAiFrames(std::string* error) {
    return ai_input_->Pause(error);
}

bool VideoPipeline::ResumeAiFrames(std::string* error) {
    return ai_input_->Resume(error);
}

bool VideoPipeline::ReconfigureAiInput(const AiInputConfig& next, std::string* error) {
    return ai_input_->Reconfigure(next, error);
}

nlohmann::json VideoPipeline::ProbeRegionCapability(std::string* error) {
    if (!rgn_manager_) {
        *error = "VENC RGN manager is unavailable";
        return {{"line", false}, {"cover", false}, {"implemented", false}};
    }
    return rgn_manager_->Probe(error);
}

bool VideoPipeline::SetOsdMode(const std::string& mode, std::string* error) {
    if (!rgn_manager_) {
        *error = "VENC RGN manager is unavailable";
        return false;
    }
    return rgn_manager_->SetMode(mode, error);
}

bool VideoPipeline::UpdateRegions(const std::vector<OsdRegion>& regions, int ttl_ms,
                                  std::string* error) {
    if (!rgn_manager_) {
        *error = "VENC RGN manager is unavailable";
        return false;
    }
    return rgn_manager_->Update(regions, ttl_ms, error);
}

void VideoPipeline::FetchLoop() {
    constexpr std::size_t kMaxPacks = 8;
    std::vector<VENC_PACK_S> packs(kMaxPacks);
    std::uint64_t consecutive_timeouts = 0;
    while (running_.load()) {
        std::fill(packs.begin(), packs.end(), VENC_PACK_S{});
        VENC_STREAM_S stream{};
        stream.pstPack = packs.data();
        RK_S32 result = RK_MPI_VENC_GetStream(config_.video.venc_channel_id, &stream, 500);
        if (result != RK_SUCCESS) {
            if (!running_.load()) break;
            stats_.timeouts.fetch_add(1);
            ++consecutive_timeouts;
            ReportTimeout(consecutive_timeouts);
            continue;
        }
        consecutive_timeouts = 0;
        const std::size_t pack_count = std::min<std::size_t>(
            stream.u32PackCount == 0 ? 1 : stream.u32PackCount, kMaxPacks);
        bool keyframe = false;
        bool write_failed = false;
        std::vector<std::uint8_t> access_unit;
        for (std::size_t i = 0; i < pack_count; ++i) {
            VENC_PACK_S& pack = packs[i];
            void* base = RK_MPI_MB_Handle2VirAddr(pack.pMbBlk);
            if (base == nullptr || pack.u32Len == 0) continue;
            // RKMPI returns pMbBlk at the valid payload start on RV1106. Its samples and the
            // existing AIPC path write u32Len bytes directly; applying u32Offset corrupts
            // circular-buffer packets on this SDK release.
            const auto* data = static_cast<const unsigned char*>(base);
            access_unit.insert(access_unit.end(), data, data + pack.u32Len);
            if (output_ != nullptr && std::fwrite(data, 1, pack.u32Len, output_) != pack.u32Len) {
                write_failed = true;
                break;
            }
            stats_.bytes.fetch_add(pack.u32Len);
            stats_.last_pts.store(pack.u64PTS);
            keyframe = keyframe ||
                       pack.DataType.enH264EType == H264E_NALU_IDRSLICE ||
                       pack.DataType.enH264EType == H264E_NALU_ISLICE;
        }
        stats_.packets.fetch_add(1);
        if (keyframe) stats_.keyframes.fetch_add(1);
        result = RK_MPI_VENC_ReleaseStream(config_.video.venc_channel_id, &stream);
        if (result != RK_SUCCESS) {
            stats_.errors.fetch_add(1);
            ReportFatal(MpiError("RK_MPI_VENC_ReleaseStream", result));
            break;
        }
        if (write_failed) {
            stats_.errors.fetch_add(1);
            ReportFatal("video output write failed: " + std::string(std::strerror(errno)));
            break;
        }
        if (ipc_publisher_ && !access_unit.empty()) {
            EncodedVideoFrame frame;
            frame.data = std::move(access_unit);
            frame.pts = packs[0].u64PTS;
            frame.sequence = ++ipc_sequence_;
            frame.keyframe = keyframe;
            if (ipc_publisher_->Enqueue(std::move(frame))) {
                RK_MPI_VENC_RequestIDR(config_.video.venc_channel_id, RK_TRUE);
            }
        }
        if (output_ != nullptr && stats_.packets.load() % 30 == 0) std::fflush(output_);
        if (keyframe && !ready_reported_.exchange(true)) {
            events_->Emit("StreamReady",
                          {{"media", "video"}, {"codec", "h264"},
                           {"width", config_.video.width}, {"height", config_.video.height},
                           {"fps", config_.video.fps}});
        }
    }
}

void VideoPipeline::ReportTimeout(std::uint64_t count) {
    if (count == static_cast<std::uint64_t>(config_.runtime.warning_timeout_count)) {
        events_->Emit("Warning", {{"media", "video"}, {"reason", "get_stream_timeout"},
                                   {"consecutive_timeouts", count}});
    } else if (count == static_cast<std::uint64_t>(config_.runtime.stalled_timeout_count)) {
        events_->Emit("StreamStalled",
                      {{"media", "video"}, {"consecutive_timeouts", count}});
    } else if (count >= static_cast<std::uint64_t>(config_.runtime.fatal_timeout_count)) {
        ReportFatal("video stream did not recover after consecutive timeouts");
    }
}

void VideoPipeline::ReportFatal(const std::string& message) {
    if (fatal_reported_.exchange(true)) return;
    events_->Emit("FatalError", {{"media", "video"}, {"message", message}});
    fatal_callback_(message);
}

void VideoPipeline::Stop() {
    running_.store(false);
    if (fetch_thread_.joinable()) fetch_thread_.join();
    if (ipc_publisher_) ipc_publisher_->Stop();
    if (ai_input_) ai_input_->Stop();
    if (output_ != nullptr) std::fflush(output_);
}

nlohmann::json VideoPipeline::Stats() const {
    auto stats = stats_.Snapshot();
    stats["ipc_frames"] = ipc_publisher_ ? ipc_publisher_->Frames() : 0;
    stats["ipc_bytes"] = ipc_publisher_ ? ipc_publisher_->Bytes() : 0;
    stats["ipc_drops"] = ipc_publisher_ ? ipc_publisher_->Drops() : 0;
    stats["ipc_errors"] = ipc_publisher_ ? ipc_publisher_->Errors() : 0;
    stats["ai_input"] = ai_input_ ? ai_input_->Stats() : nlohmann::json::object();
    return stats;
}

void VideoPipeline::Deinit() {
    Stop();
    if (rgn_manager_) {
        rgn_manager_->Deinit();
        rgn_manager_.reset();
    }
    if (vpss_venc_bound_) {
        RK_MPI_SYS_UnBind(&vpss_channel_, &venc_channel_);
        vpss_venc_bound_ = false;
    }
    if (vi_vpss_bound_) {
        RK_MPI_SYS_UnBind(&vi_channel_, &vpss_group_);
        vi_vpss_bound_ = false;
    }
    if (venc_receiving_) {
        RK_MPI_VENC_StopRecvFrame(config_.video.venc_channel_id);
        venc_receiving_ = false;
    }
    if (venc_created_) {
        RK_MPI_VENC_DestroyChn(config_.video.venc_channel_id);
        venc_created_ = false;
    }
    if (vpss_started_) {
        RK_MPI_VPSS_StopGrp(config_.vpss.group_id);
        vpss_started_ = false;
    }
    if (ai_input_) {
        ai_input_->Deinit();
        ai_input_.reset();
    }
    if (vpss_channel_enabled_) {
        RK_MPI_VPSS_DisableChn(config_.vpss.group_id, config_.vpss.channel_id);
        vpss_channel_enabled_ = false;
    }
    if (vpss_group_created_) {
        RK_MPI_VPSS_DestroyGrp(config_.vpss.group_id);
        vpss_group_created_ = false;
    }
    if (vi_channel_enabled_) {
        RK_MPI_VI_DisableChn(config_.vi.device_id, config_.vi.channel_id);
        vi_channel_enabled_ = false;
    }
    if (vi_device_enabled_) {
        RK_MPI_VI_DisableDev(config_.vi.device_id);
        vi_device_enabled_ = false;
    }
    if (output_ != nullptr) {
        std::fclose(output_);
        output_ = nullptr;
    }
}

}  // namespace media_worker
