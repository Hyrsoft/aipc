#include "audio_pipeline.h"

#include <cerrno>
#include <cstring>

#include <sstream>
#include <utility>

#include "rk_mpi_aenc.h"
#include "rk_mpi_ai.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"

namespace media_worker {
namespace {

std::string MpiError(const char* operation, RK_S32 result) {
    std::ostringstream message;
    message << operation << " failed: 0x" << std::hex << result;
    return message.str();
}

}  // namespace

AudioPipeline::AudioPipeline(const WorkerConfig& config, EventEmitter* events,
                             FatalCallback fatal_callback)
    : config_(config), events_(events), fatal_callback_(std::move(fatal_callback)) {}

AudioPipeline::~AudioPipeline() {
    Deinit();
}

bool AudioPipeline::Init(std::string* error) {
    if (!config_.audio.output_path.empty()) {
        output_ = std::fopen(config_.audio.output_path.c_str(), "wb");
        if (output_ == nullptr) {
            *error = "cannot open audio debug output " + config_.audio.output_path + ": " +
                     std::strerror(errno);
            return false;
        }
    }

    AIO_ATTR_S ai_attr{};
    std::snprintf(reinterpret_cast<char*>(ai_attr.u8CardName), sizeof(ai_attr.u8CardName),
                  "%s", config_.audio.card_name.c_str());
    ai_attr.soundCard.channels = config_.audio.device_channels;
    ai_attr.soundCard.sampleRate = config_.audio.device_sample_rate;
    ai_attr.soundCard.bitWidth = AUDIO_BIT_WIDTH_16;
    ai_attr.enSamplerate = static_cast<AUDIO_SAMPLE_RATE_E>(config_.audio.sample_rate);
    ai_attr.enBitwidth = AUDIO_BIT_WIDTH_16;
    ai_attr.enSoundmode = AUDIO_SOUND_MODE_MONO;
    ai_attr.u32FrmNum = config_.audio.buffer_count;
    ai_attr.u32PtNumPerFrm = config_.audio.frame_samples;
    ai_attr.u32ChnCnt = config_.audio.device_channels;

    RK_S32 result = RK_MPI_AI_SetPubAttr(config_.audio.device_id, &ai_attr);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_AI_SetPubAttr", result);
        return false;
    }
    result = RK_MPI_AI_Enable(config_.audio.device_id);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_AI_Enable", result);
        return false;
    }
    ai_enabled_ = true;

    AI_CHN_PARAM_S channel_params{};
    channel_params.enLoopbackMode = AUDIO_LOOPBACK_NONE;
    channel_params.s32UsrFrmDepth = 1;
    result = RK_MPI_AI_SetChnParam(config_.audio.device_id, config_.audio.channel_id,
                                   &channel_params);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_AI_SetChnParam", result);
        return false;
    }
    RK_MPI_AI_SetTrackMode(config_.audio.device_id, AUDIO_TRACK_FRONT_LEFT);
    result = RK_MPI_AI_EnableChn(config_.audio.device_id, config_.audio.channel_id);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_AI_EnableChn", result);
        return false;
    }
    ai_channel_enabled_ = true;
    if (config_.audio.device_sample_rate != config_.audio.sample_rate) {
        result = RK_MPI_AI_EnableReSmp(
            config_.audio.device_id, config_.audio.channel_id,
            static_cast<AUDIO_SAMPLE_RATE_E>(config_.audio.sample_rate));
        if (result != RK_SUCCESS) {
            *error = MpiError("RK_MPI_AI_EnableReSmp", result);
            return false;
        }
        ai_resample_enabled_ = true;
    }
    events_->Emit("BootProgress", {{"stage", "audio_input_ready"}});

    AENC_CHN_ATTR_S aenc_attr{};
    aenc_attr.enType = RK_AUDIO_ID_PCM_ALAW;
    aenc_attr.u32BufCount = config_.audio.buffer_count;
    aenc_attr.u32Depth = config_.audio.buffer_count;
    aenc_attr.stCodecAttr.enType = RK_AUDIO_ID_PCM_ALAW;
    aenc_attr.stCodecAttr.enBitwidth = AUDIO_BIT_WIDTH_16;
    aenc_attr.stCodecAttr.u32Channels = config_.audio.channels;
    aenc_attr.stCodecAttr.u32SampleRate = config_.audio.sample_rate;
    aenc_attr.stCodecAttr.u32BitPerCodedSample = 8;
    aenc_attr.stCodecAttr.u32Bitrate = config_.audio.bitrate;
    result = RK_MPI_AENC_CreateChn(config_.audio.aenc_channel_id, &aenc_attr);
    if (result != RK_SUCCESS) {
        *error = MpiError("RK_MPI_AENC_CreateChn", result);
        return false;
    }
    aenc_created_ = true;

    ai_channel_ = {RK_ID_AI, config_.audio.device_id, config_.audio.channel_id};
    aenc_channel_ = {RK_ID_AENC, 0, config_.audio.aenc_channel_id};
    result = RK_MPI_SYS_Bind(&ai_channel_, &aenc_channel_);
    if (result != RK_SUCCESS) {
        *error = MpiError("bind AI to AENC", result);
        return false;
    }
    ai_aenc_bound_ = true;
    events_->Emit("BootProgress", {{"stage", "audio_bound"}});
    return true;
}

bool AudioPipeline::Start(std::string* error) {
    if (running_.exchange(true)) {
        *error = "audio fetch loop already running";
        return false;
    }
    try {
        fetch_thread_ = std::thread(&AudioPipeline::FetchLoop, this);
    } catch (const std::exception& exception) {
        running_.store(false);
        *error = std::string("cannot start audio fetch thread: ") + exception.what();
        return false;
    }
    return true;
}

void AudioPipeline::FetchLoop() {
    std::uint64_t consecutive_timeouts = 0;
    while (running_.load()) {
        AUDIO_STREAM_S stream{};
        RK_S32 result = RK_MPI_AENC_GetStream(config_.audio.aenc_channel_id, &stream, 500);
        if (result != RK_SUCCESS) {
            if (!running_.load()) break;
            stats_.timeouts.fetch_add(1);
            ++consecutive_timeouts;
            ReportTimeout(consecutive_timeouts);
            continue;
        }
        consecutive_timeouts = 0;
        void* data = RK_MPI_MB_Handle2VirAddr(stream.pMbBlk);
        bool write_failed = false;
        if (data != nullptr && stream.u32Len > 0) {
            if (output_ != nullptr && std::fwrite(data, 1, stream.u32Len, output_) != stream.u32Len) {
                write_failed = true;
            } else {
                stats_.packets.fetch_add(1);
                stats_.bytes.fetch_add(stream.u32Len);
                stats_.last_pts.store(stream.u64TimeStamp);
            }
        }
        result = RK_MPI_AENC_ReleaseStream(config_.audio.aenc_channel_id, &stream);
        if (result != RK_SUCCESS) {
            stats_.errors.fetch_add(1);
            ReportFatal(MpiError("RK_MPI_AENC_ReleaseStream", result));
            break;
        }
        if (write_failed) {
            stats_.errors.fetch_add(1);
            ReportFatal("audio output write failed: " + std::string(std::strerror(errno)));
            break;
        }
        if (output_ != nullptr && stats_.packets.load() % 50 == 0) std::fflush(output_);
        if (stream.u32Len > 0 && !ready_reported_.exchange(true)) {
            events_->Emit("StreamReady",
                          {{"media", "audio"}, {"codec", "g711a"},
                           {"sample_rate", config_.audio.sample_rate},
                           {"channels", config_.audio.channels}});
        }
    }
}

void AudioPipeline::ReportTimeout(std::uint64_t count) {
    if (count == static_cast<std::uint64_t>(config_.runtime.warning_timeout_count)) {
        events_->Emit("Warning", {{"media", "audio"}, {"reason", "get_stream_timeout"},
                                   {"consecutive_timeouts", count}});
    } else if (count == static_cast<std::uint64_t>(config_.runtime.stalled_timeout_count)) {
        events_->Emit("StreamStalled",
                      {{"media", "audio"}, {"consecutive_timeouts", count}});
    } else if (count >= static_cast<std::uint64_t>(config_.runtime.fatal_timeout_count)) {
        ReportFatal("audio stream did not recover after consecutive timeouts");
    }
}

void AudioPipeline::ReportFatal(const std::string& message) {
    if (fatal_reported_.exchange(true)) return;
    events_->Emit("FatalError", {{"media", "audio"}, {"message", message}});
    fatal_callback_(message);
}

void AudioPipeline::Stop() {
    running_.store(false);
    if (fetch_thread_.joinable()) fetch_thread_.join();
    if (output_ != nullptr) std::fflush(output_);
}

void AudioPipeline::Deinit() {
    Stop();
    if (ai_aenc_bound_) {
        RK_MPI_SYS_UnBind(&ai_channel_, &aenc_channel_);
        ai_aenc_bound_ = false;
    }
    if (aenc_created_) {
        RK_MPI_AENC_DestroyChn(config_.audio.aenc_channel_id);
        aenc_created_ = false;
    }
    if (ai_resample_enabled_) {
        RK_MPI_AI_DisableReSmp(config_.audio.device_id, config_.audio.channel_id);
        ai_resample_enabled_ = false;
    }
    if (ai_channel_enabled_) {
        RK_MPI_AI_DisableChn(config_.audio.device_id, config_.audio.channel_id);
        ai_channel_enabled_ = false;
    }
    if (ai_enabled_) {
        RK_MPI_AI_Disable(config_.audio.device_id);
        ai_enabled_ = false;
    }
    if (output_ != nullptr) {
        std::fclose(output_);
        output_ = nullptr;
    }
}

}  // namespace media_worker
