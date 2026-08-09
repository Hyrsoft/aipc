#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <memory>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "aipc/native/aipf.h"
#include "aipc/native/aipv2.h"
#include "aipc/native/io.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_cal.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vdec.h"
#include "rk_mpi_vpss.h"

namespace {

using aipc::native::AiFitMode;
using aipc::native::AiFrame;
using aipc::native::AiFrameTransform;
using aipc::native::EncodedAccessUnit;

struct Options {
    int input_fd = 3;
    int output_fd = 4;
    int control_fd = 6;
    std::string source_id;
    std::string generation;
};

struct Config {
    int source_width = 0;
    int source_height = 0;
    int source_fps = 25;
    int output_width = 640;
    int output_height = 360;
    int output_fps = 10;
    int vdec_channel = 0;
    int vpss_group = 1;
    int vpss_channel = 1;
    int buffer_count = 2;
    int depth = 1;
    std::string fit_mode = "contain";
};

bool ParseConfigValue(const nlohmann::json& value, Config* config, std::string* error);

void Emit(const char* type, const nlohmann::json& body = {}) {
    nlohmann::json event = body;
    event["type"] = type;
    std::cout << event.dump() << '\n' << std::flush;
}

std::string MpiError(const char* operation, RK_S32 result) {
    char code[32];
    std::snprintf(code, sizeof(code), "0x%x", result);
    return std::string(operation) + " failed: " + code;
}

bool ParseOptions(int argc, char** argv, Options* options, std::string* error) {
    for (int index = 1; index < argc; ++index) {
        const std::string name = argv[index];
        if (index + 1 >= argc) {
            *error = "missing value for " + name;
            return false;
        }
        const std::string value = argv[++index];
        if (name == "--input-fd") options->input_fd = std::stoi(value);
        else if (name == "--output-fd") options->output_fd = std::stoi(value);
        else if (name == "--control-fd") options->control_fd = std::stoi(value);
        else if (name == "--processed-output-fd") continue;
        else if (name == "--source-id") options->source_id = value;
        else if (name == "--generation") options->generation = value;
        else {
            *error = "unknown option " + name;
            return false;
        }
    }
    return true;
}

bool ReadConfig(int fd, Config* config, std::string* error) {
    std::uint8_t header[4];
    if (!aipc::native::ReadAll(fd, header, sizeof(header))) {
        *error = "cannot read processor control header";
        return false;
    }
    const std::size_t length = aipc::native::ReadU32(header);
    if (length == 0 || length > aipc::native::kMaxJsonMessageBytes) {
        *error = "invalid processor control length";
        return false;
    }
    std::string payload(length, '\0');
    if (!aipc::native::ReadAll(fd, payload.data(), payload.size())) {
        *error = "truncated processor control message";
        return false;
    }
    try {
        const auto value = nlohmann::json::parse(payload);
        if (value.value("command", "") != "configure") {
            *error = "first processor command must be configure";
            return false;
        }
        return ParseConfigValue(value, config, error);
    } catch (const std::exception& exception) {
        *error = std::string("invalid processor configuration: ") + exception.what();
        return false;
    }
}

bool ParseConfigValue(const nlohmann::json& value, Config* config, std::string* error) {
    try {
        const auto& input = value.at("input");
        const auto& output = value.at("output");
        config->source_width = input.value("width", 0);
        config->source_height = input.value("height", 0);
        config->source_fps = input.value("fps", 25);
        config->output_width = output.value("width", 640);
        config->output_height = output.value("height", 360);
        config->output_fps = output.value("fps", 10);
        config->vdec_channel = value.value("vdec_channel", 0);
        config->vpss_group = value.value("vpss_group", 1);
        config->vpss_channel = output.value("channel_id", 1);
        config->buffer_count = output.value("buffer_count", 2);
        config->depth = output.value("depth", 1);
        config->fit_mode = output.value("fit_mode", "contain");
    } catch (const std::exception& exception) {
        *error = std::string("invalid processor configuration: ") + exception.what();
        return false;
    }
    if (config->source_width <= 0 || config->source_height <= 0 ||
        config->output_width < 384 || config->output_height < 256 ||
        config->output_width % 2 != 0 || config->output_height % 2 != 0 ||
        config->source_fps <= 0 || config->output_fps <= 0 ||
        config->vdec_channel < 0 || config->vdec_channel > 7 ||
        config->vpss_group < 0 || config->vpss_group > 7 ||
        (config->fit_mode != "stretch" && config->fit_mode != "contain" &&
         config->fit_mode != "cover")) {
        *error = "processor dimensions, FPS, or fit mode are invalid";
        return false;
    }
    return true;
}

AiFrameTransform ComputeTransform(const Config& config) {
    AiFrameTransform transform;
    transform.crop_width = config.source_width;
    transform.crop_height = config.source_height;
    if (config.fit_mode == "contain") {
        const double scale = std::min(
            static_cast<double>(config.output_width) / config.source_width,
            static_cast<double>(config.output_height) / config.source_height);
        const int content_width =
            std::max(4, static_cast<int>(std::floor(config.source_width * scale)) & ~3);
        const int content_height =
            std::max(4, static_cast<int>(std::floor(config.source_height * scale)) & ~3);
        transform.pad_left = ((config.output_width - content_width) / 2) & ~1;
        transform.pad_top = ((config.output_height - content_height) / 2) & ~1;
        transform.pad_right = config.output_width - content_width - transform.pad_left;
        transform.pad_bottom = config.output_height - content_height - transform.pad_top;
    } else if (config.fit_mode == "cover") {
        const double source_ratio =
            static_cast<double>(config.source_width) / config.source_height;
        const double target_ratio =
            static_cast<double>(config.output_width) / config.output_height;
        if (source_ratio > target_ratio) {
            transform.crop_width = static_cast<int>(config.source_height * target_ratio) & ~1;
            transform.crop_x = (config.source_width - transform.crop_width) / 2;
        } else if (source_ratio < target_ratio) {
            transform.crop_height = static_cast<int>(config.source_width / target_ratio) & ~1;
            transform.crop_y = (config.source_height - transform.crop_height) / 2;
        }
    }
    return transform;
}

RK_S32 FreeStreamBuffer(void* opaque) {
    std::free(opaque);
    return RK_SUCCESS;
}

class Pipeline {
public:
    Pipeline(const Config& config, int output_fd)
        : config_(config), output_fd_(output_fd), transform_(ComputeTransform(config)) {}

    ~Pipeline() { Shutdown(); }

    bool Start(std::string* error) {
        RK_S32 result = RK_MPI_SYS_Init();
        if (result != RK_SUCCESS) return Fail("RK_MPI_SYS_Init", result, error);
        sys_ready_ = true;

        VPSS_GRP_ATTR_S group{};
        group.u32MaxW = static_cast<RK_U32>(config_.source_width);
        group.u32MaxH = static_cast<RK_U32>(config_.source_height);
        group.enPixelFormat = RK_FMT_YUV420SP;
        group.enDynamicRange = DYNAMIC_RANGE_SDR8;
        group.enCompressMode = COMPRESS_MODE_NONE;
        group.stFrameRate.s32SrcFrameRate = config_.source_fps;
        group.stFrameRate.s32DstFrameRate = config_.source_fps;
        result = RK_MPI_VPSS_CreateGrp(config_.vpss_group, &group);
        if (result != RK_SUCCESS) return Fail("RK_MPI_VPSS_CreateGrp", result, error);
        vpss_created_ = true;

        VPSS_CHN_ATTR_S channel{};
        channel.enChnMode = VPSS_CHN_MODE_USER;
        channel.enDynamicRange = DYNAMIC_RANGE_SDR8;
        channel.enPixelFormat = RK_FMT_YUV420SP;
        channel.enCompressMode = COMPRESS_MODE_NONE;
        channel.stFrameRate.s32SrcFrameRate = config_.source_fps;
        channel.stFrameRate.s32DstFrameRate = config_.output_fps;
        channel.u32Depth = static_cast<RK_U32>(config_.depth);
        channel.u32FrameBufCnt = static_cast<RK_U32>(config_.buffer_count);
        channel.stAspectRatio.enMode = ASPECT_RATIO_NONE;
        channel.u32Width = static_cast<RK_U32>(
            config_.fit_mode == "contain"
                ? config_.output_width - transform_.pad_left - transform_.pad_right
                : config_.output_width);
        channel.u32Height = static_cast<RK_U32>(
            config_.fit_mode == "contain"
                ? config_.output_height - transform_.pad_top - transform_.pad_bottom
                : config_.output_height);
        result = RK_MPI_VPSS_SetChnAttr(config_.vpss_group, config_.vpss_channel, &channel);
        if (result != RK_SUCCESS) return Fail("RK_MPI_VPSS_SetChnAttr", result, error);

        if (config_.fit_mode == "cover") {
            VPSS_CROP_INFO_S crop{};
            crop.bEnable = RK_TRUE;
            crop.enCropCoordinate = VPSS_CROP_ABS_COOR;
            crop.stCropRect.s32X = transform_.crop_x;
            crop.stCropRect.s32Y = transform_.crop_y;
            crop.stCropRect.u32Width = static_cast<RK_U32>(transform_.crop_width);
            crop.stCropRect.u32Height = static_cast<RK_U32>(transform_.crop_height);
            result = RK_MPI_VPSS_SetChnCrop(config_.vpss_group, config_.vpss_channel, &crop);
            if (result != RK_SUCCESS) return Fail("RK_MPI_VPSS_SetChnCrop", result, error);
        }
        result = RK_MPI_VPSS_EnableChn(config_.vpss_group, config_.vpss_channel);
        if (result != RK_SUCCESS) return Fail("RK_MPI_VPSS_EnableChn", result, error);
        vpss_channel_enabled_ = true;
        result = RK_MPI_VPSS_StartGrp(config_.vpss_group);
        if (result != RK_SUCCESS) return Fail("RK_MPI_VPSS_StartGrp", result, error);
        vpss_started_ = true;

        VDEC_CHN_ATTR_S decoder{};
        decoder.enMode = VIDEO_MODE_STREAM;
        decoder.enType = RK_VIDEO_ID_AVC;
        decoder.u32PicWidth = static_cast<RK_U32>(config_.source_width);
        decoder.u32PicHeight = static_cast<RK_U32>(config_.source_height);
        decoder.u32FrameBufCnt = 8;
        decoder.u32StreamBufCnt = 8;
        decoder.u32StreamBufSize = 2 * 1024 * 1024;
        decoder.u32FrameBufDepth = 2;
        decoder.stVdecVideoAttr.bTemporalMvpEnable = RK_FALSE;

        VDEC_PIC_BUF_ATTR_S picture{};
        picture.enCodecType = RK_VIDEO_ID_AVC;
        picture.stPicBufAttr.u32Width = decoder.u32PicWidth;
        picture.stPicBufAttr.u32Height = decoder.u32PicHeight;
        picture.stPicBufAttr.enPixelFormat = RK_FMT_YUV420SP;
        picture.stPicBufAttr.enCompMode = COMPRESS_MODE_NONE;
        MB_PIC_CAL_S calculated{};
        result = RK_MPI_CAL_VDEC_GetPicBufferSize(&picture, &calculated);
        if (result != RK_SUCCESS) {
            return Fail("RK_MPI_CAL_VDEC_GetPicBufferSize", result, error);
        }
        decoder.u32FrameBufSize = calculated.u32MBSize;
        result = RK_MPI_VDEC_CreateChn(config_.vdec_channel, &decoder);
        if (result != RK_SUCCESS) return Fail("RK_MPI_VDEC_CreateChn", result, error);
        vdec_created_ = true;

        VDEC_CHN_PARAM_S decoder_param{};
        decoder_param.enType = RK_VIDEO_ID_AVC;
        decoder_param.stVdecVideoParam.enCompressMode = COMPRESS_MODE_NONE;
        decoder_param.stVdecVideoParam.enOutputOrder = VIDEO_OUTPUT_ORDER_DEC;
        result = RK_MPI_VDEC_SetChnParam(config_.vdec_channel, &decoder_param);
        if (result != RK_SUCCESS) return Fail("RK_MPI_VDEC_SetChnParam", result, error);
        result = RK_MPI_VDEC_StartRecvStream(config_.vdec_channel);
        if (result != RK_SUCCESS) return Fail("RK_MPI_VDEC_StartRecvStream", result, error);
        vdec_started_ = true;

        return true;
    }

    bool Submit(const EncodedAccessUnit& input, std::string* error) {
        if (input.discontinuity()) {
            RK_MPI_VDEC_ResetChn(config_.vdec_channel);
            pending_sequences_.clear();
        }
        if (input.end_of_stream()) return Finish(error);

        void* bytes = std::malloc(input.data.size());
        if (bytes == nullptr) {
            *error = "cannot allocate encoded access unit";
            return false;
        }
        std::memcpy(bytes, input.data.data(), input.data.size());
        MB_EXT_CONFIG_S external{};
        external.pFreeCB = FreeStreamBuffer;
        external.pOpaque = bytes;
        external.pu8VirAddr = static_cast<RK_U8*>(bytes);
        external.u64Size = input.data.size();
        MB_BLK block = nullptr;
        RK_S32 result = RK_MPI_SYS_CreateMB(&block, &external);
        if (result != RK_SUCCESS) {
            std::free(bytes);
            return Fail("RK_MPI_SYS_CreateMB", result, error);
        }
        VDEC_STREAM_S stream{};
        stream.pMbBlk = block;
        stream.u32Len = static_cast<RK_U32>(input.data.size());
        stream.u64PTS = input.pts;
        // AIPV2 carries one complete H.264 access unit per message. Marking
        // the packet as an end-of-frame lets VDEC emit frames for elementary
        // streams that do not carry MP4 sample boundaries, while remaining
        // correct for MP4-derived access units.
        stream.bEndOfFrame = RK_TRUE;
        stream.bBypassMbBlk = RK_TRUE;
        result = RK_MPI_VDEC_SendStream(config_.vdec_channel, &stream, 200);
        RK_MPI_MB_ReleaseMB(block);
        if (result != RK_SUCCESS) return Fail("RK_MPI_VDEC_SendStream", result, error);
        pending_sequences_[input.pts] = input.sequence;
        ++submitted_frames_;
        const bool drained = Drain(100, error);
        if (submitted_frames_ == 1 || submitted_frames_ % 30 == 0) EmitDecoderStatus();
        return drained;
    }

    bool Finish(std::string* error) {
        VDEC_STREAM_S stream{};
        stream.bEndOfStream = RK_TRUE;
        stream.bEndOfFrame = RK_TRUE;
        RK_S32 result = RK_MPI_VDEC_SendStream(config_.vdec_channel, &stream, 200);
        if (result != RK_SUCCESS) return Fail("RK_MPI_VDEC_SendStream(EOS)", result, error);
        for (int count = 0; count < 20; ++count) {
            if (!Drain(100, error)) return false;
        }
        return true;
    }

    void Flush() {
        RK_MPI_VDEC_ResetChn(config_.vdec_channel);
        pending_sequences_.clear();
    }

private:
    bool Drain(int timeout_ms, std::string* error) {
        VIDEO_FRAME_INFO_S decoded{};
        const RK_S32 decode_result = RK_MPI_VDEC_GetFrame(
            config_.vdec_channel, &decoded, timeout_ms);
        if (decode_result != RK_SUCCESS) return true;
        const RK_S32 send_result = RK_MPI_VPSS_SendFrame(
            config_.vpss_group, 0, &decoded, timeout_ms);
        RK_MPI_VDEC_ReleaseFrame(config_.vdec_channel, &decoded);
        if (send_result != RK_SUCCESS) {
            *error = MpiError("RK_MPI_VPSS_SendFrame", send_result);
            return false;
        }
        VIDEO_FRAME_INFO_S frame{};
        const RK_S32 result = RK_MPI_VPSS_GetChnFrame(
            config_.vpss_group, config_.vpss_channel, &frame, timeout_ms);
        if (result != RK_SUCCESS) return true;
        const bool ok = Publish(frame, error);
        RK_MPI_VPSS_ReleaseChnFrame(config_.vpss_group, config_.vpss_channel, &frame);
        return ok;
    }

    bool Publish(const VIDEO_FRAME_INFO_S& frame, std::string* error) {
        const auto width = frame.stVFrame.u32Width;
        const auto height = frame.stVFrame.u32Height;
        const auto stride = frame.stVFrame.u32VirWidth == 0 ? width : frame.stVFrame.u32VirWidth;
        const auto vir_height = frame.stVFrame.u32VirHeight == 0
                                    ? height
                                    : frame.stVFrame.u32VirHeight;
        const std::size_t expected = static_cast<std::size_t>(stride) * vir_height * 3 / 2;
        const std::size_t available = RK_MPI_MB_GetSize(frame.stVFrame.pMbBlk);
        const auto* address = static_cast<const std::uint8_t*>(
            RK_MPI_MB_Handle2VirAddr(frame.stVFrame.pMbBlk));
        if (address == nullptr || expected == 0 || available < expected) {
            *error = "VPSS returned an invalid NV12 frame";
            return false;
        }
        AiFrame output;
        output.pts = frame.stVFrame.u64PTS;
        auto sequence = pending_sequences_.find(output.pts);
        output.sequence = sequence == pending_sequences_.end() ? ++fallback_sequence_
                                                               : sequence->second;
        if (sequence != pending_sequences_.end()) pending_sequences_.erase(sequence);
        output.main_width = config_.source_width;
        output.main_height = config_.source_height;
        output.transform = transform_;
        output.fit_mode = config_.fit_mode == "contain"
                              ? AiFitMode::kContain
                              : config_.fit_mode == "cover" ? AiFitMode::kCover
                                                             : AiFitMode::kStretch;
        if (config_.fit_mode == "contain") {
            const std::size_t y_size =
                static_cast<std::size_t>(config_.output_width) * config_.output_height;
            output.data.assign(y_size + y_size / 2, 128);
            std::fill(output.data.begin(), output.data.begin() + y_size, 16);
            const int content_width =
                config_.output_width - transform_.pad_left - transform_.pad_right;
            const int content_height =
                config_.output_height - transform_.pad_top - transform_.pad_bottom;
            const auto* source_uv = address + static_cast<std::size_t>(stride) * vir_height;
            auto* target_y = output.data.data() +
                             static_cast<std::size_t>(transform_.pad_top) * config_.output_width +
                             transform_.pad_left;
            auto* target_uv = output.data.data() + y_size +
                              static_cast<std::size_t>(transform_.pad_top / 2) *
                                  config_.output_width +
                              transform_.pad_left;
            for (int row = 0; row < content_height; ++row) {
                std::memcpy(target_y + static_cast<std::size_t>(row) * config_.output_width,
                            address + static_cast<std::size_t>(row) * stride,
                            content_width);
            }
            for (int row = 0; row < content_height / 2; ++row) {
                std::memcpy(target_uv + static_cast<std::size_t>(row) * config_.output_width,
                            source_uv + static_cast<std::size_t>(row) * stride,
                            content_width);
            }
            output.width = config_.output_width;
            output.height = config_.output_height;
            output.y_stride = config_.output_width;
            output.uv_stride = config_.output_width;
            output.height_stride = config_.output_height;
        } else {
            output.data.assign(address, address + expected);
            output.width = width;
            output.height = height;
            output.y_stride = stride;
            output.uv_stride = stride;
            output.height_stride = vir_height;
        }
        const auto encoded = aipc::native::EncodeAipfFrame(output);
        if (!aipc::native::WriteAll(output_fd_, encoded.data(), encoded.size())) {
            *error = "write AIPF output failed";
            return false;
        }
        ++frames_;
        if (frames_ == 1) Emit("ready", {{"width", output.width}, {"height", output.height}});
        return true;
    }

    void EmitDecoderStatus() {
        VDEC_CHN_STATUS_S status{};
        const RK_S32 result = RK_MPI_VDEC_QueryStatus(config_.vdec_channel, &status);
        if (result != RK_SUCCESS) {
            Emit("decoder_status", {{"query_error", MpiError("RK_MPI_VDEC_QueryStatus", result)}});
            return;
        }
        const auto& decode_error = status.stVdecDecErr;
        Emit("decoder_status",
             {{"submitted", submitted_frames_},
              {"received", status.u32RecvStreamFrames},
              {"decoded", status.u32DecodeStreamFrames},
              {"left_stream_frames", status.u32LeftStreamFrames},
              {"left_pictures", status.u32LeftPics},
              {"errors",
               {{"format", decode_error.s32FormatErr},
                {"picture_size", decode_error.s32PicSizeErrSet},
                {"unsupported", decode_error.s32StreamUnsprt},
                {"packet", decode_error.s32PackErr},
                {"protocol", decode_error.s32PrtclNumErrSet},
                {"reference", decode_error.s32RefErrSet},
                {"picture_buffer", decode_error.s32PicBufSizeErrSet},
                {"stream_size", decode_error.s32StreamSizeOver}}}});
    }

    bool Fail(const char* operation, RK_S32 result, std::string* error) {
        *error = MpiError(operation, result);
        return false;
    }

    void Shutdown() {
        if (bound_) {
            MPP_CHN_S source{RK_ID_VDEC, 0, config_.vdec_channel};
            MPP_CHN_S destination{RK_ID_VPSS, config_.vpss_group, 0};
            RK_MPI_SYS_UnBind(&source, &destination);
            bound_ = false;
        }
        if (vdec_started_) RK_MPI_VDEC_StopRecvStream(config_.vdec_channel);
        if (vdec_created_) RK_MPI_VDEC_DestroyChn(config_.vdec_channel);
        if (vpss_started_) RK_MPI_VPSS_StopGrp(config_.vpss_group);
        if (vpss_channel_enabled_)
            RK_MPI_VPSS_DisableChn(config_.vpss_group, config_.vpss_channel);
        if (vpss_created_) RK_MPI_VPSS_DestroyGrp(config_.vpss_group);
        if (sys_ready_) RK_MPI_SYS_Exit();
        bound_ = vdec_started_ = vdec_created_ = vpss_started_ =
            vpss_channel_enabled_ = vpss_created_ = sys_ready_ = false;
    }

    Config config_;
    int output_fd_;
    AiFrameTransform transform_;
    std::unordered_map<std::uint64_t, std::uint64_t> pending_sequences_;
    std::uint64_t fallback_sequence_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t submitted_frames_ = 0;
    bool sys_ready_ = false;
    bool vpss_created_ = false;
    bool vpss_channel_enabled_ = false;
    bool vpss_started_ = false;
    bool vdec_created_ = false;
    bool vdec_started_ = false;
    bool bound_ = false;
};

}  // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!ParseOptions(argc, argv, &options, &error)) {
        Emit("error", {{"stage", "arguments"}, {"error", error}});
        return 2;
    }
    Config config;
    if (!ReadConfig(options.control_fd, &config, &error)) {
        Emit("error", {{"stage", "configure"}, {"error", error}});
        return 2;
    }
    auto pipeline = std::make_unique<Pipeline>(config, options.output_fd);
    if (!pipeline->Start(&error)) {
        Emit("error", {{"stage", "startup"}, {"error", error}});
        return 1;
    }
    Emit("started", {{"source_id", options.source_id},
                     {"source_generation", options.generation}});
    bool stopping = false;
    while (!stopping) {
        struct pollfd descriptors[2] = {
            {options.input_fd, POLLIN | POLLHUP | POLLERR, 0},
            {options.control_fd, POLLIN | POLLHUP | POLLERR, 0},
        };
        if (poll(descriptors, 2, 100) < 0) continue;
        if (descriptors[1].revents & POLLIN) {
            std::uint8_t header[4];
            if (!aipc::native::ReadAll(options.control_fd, header, sizeof(header))) {
                Emit("error", {{"stage", "control"}, {"error", "truncated control header"}});
                return 1;
            }
            const auto length = aipc::native::ReadU32(header);
            if (length == 0 || length > aipc::native::kMaxJsonMessageBytes) {
                Emit("error", {{"stage", "control"}, {"error", "invalid control length"}});
                return 1;
            }
            std::string payload(length, '\0');
            if (!aipc::native::ReadAll(options.control_fd, payload.data(), length)) {
                Emit("error", {{"stage", "control"}, {"error", "truncated control payload"}});
                return 1;
            }
            try {
                const auto command = nlohmann::json::parse(payload);
                const auto name = command.value("command", "");
                if (name == "flush") {
                    pipeline->Flush();
                    aipc::native::WriteJsonMessage(options.control_fd,
                                                   {{"type", "ack"}, {"command", name}});
                } else if (name == "stop") {
                    aipc::native::WriteJsonMessage(options.control_fd,
                                                   {{"type", "ack"}, {"command", name}});
                    stopping = true;
                } else if (name == "reconfigure") {
                    Config replacement;
                    if (!ParseConfigValue(command, &replacement, &error)) {
                        aipc::native::WriteJsonMessage(options.control_fd,
                                                       {{"type", "error"}, {"error", error}});
                    } else {
                        pipeline.reset();
                        pipeline = std::make_unique<Pipeline>(replacement, options.output_fd);
                        if (!pipeline->Start(&error)) {
                            aipc::native::WriteJsonMessage(options.control_fd,
                                                           {{"type", "error"}, {"error", error}});
                            return 1;
                        }
                        config = replacement;
                        aipc::native::WriteJsonMessage(options.control_fd,
                                                       {{"type", "ack"}, {"command", name}});
                    }
                } else {
                    aipc::native::WriteJsonMessage(options.control_fd,
                                                   {{"type", "error"}, {"error", "unknown command"}});
                }
            } catch (const std::exception& exception) {
                aipc::native::WriteJsonMessage(
                    options.control_fd,
                    {{"type", "error"}, {"error", exception.what()}});
            }
        }
        if (stopping) break;
        if (descriptors[0].revents & (POLLHUP | POLLERR)) break;
        if (!(descriptors[0].revents & POLLIN)) continue;
        error.clear();
        auto frame = aipc::native::ReadAipv2AccessUnit(options.input_fd, &error);
        if (!frame.has_value()) {
            if (!error.empty()) {
                Emit("error", {{"stage", "input"}, {"error", error}});
                return 1;
            }
            break;
        }
        if (!pipeline->Submit(*frame, &error)) {
            Emit("error", {{"stage", "decode"}, {"error", error}});
            return 1;
        }
        if (frame->end_of_stream()) break;
    }
    Emit("stopped", {{"reason", stopping ? "control" : "eof"}});
    return 0;
}
