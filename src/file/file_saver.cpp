/**
 * @file file_saver.cpp
 * @brief 文件保存器实现 - MP4 录制和 JPEG 拍照
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#define LOG_TAG "file"

#include "file_saver.h"
#include "common/logger.h"

#include "rk_mpi_mb.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_tde.h"
#include "rk_mpi_cal.h"

#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <chrono>
#include <thread>

// FFmpeg 头文件
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
}

// ============================================================================
// 辅助函数
// ============================================================================

static std::string GenerateTimestampFilename(const std::string& prefix) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&time);
    
    char buf[64];
    snprintf(buf, sizeof(buf), "%s_%04d%02d%02d_%02d%02d%02d",
             prefix.c_str(),
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

static bool EnsureDirectory(const std::string& dir) {
    struct stat st;
    if (stat(dir.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    
    if (mkdir(dir.c_str(), 0755) == 0) {
        return true;
    }
    
    LOG_ERROR("Failed to create directory: {}", dir);
    return false;
}

/**
 * @brief 在 H.264 Annex B 流中查找 NAL 单元
 * @param data 数据指针
 * @param size 数据大小
 * @param start 输出：NAL 起始位置（不含 start code）
 * @param nal_size 输出：NAL 大小
 * @param offset 搜索起始偏移
 * @return 下一个 NAL 的搜索偏移，如果没有找到返回 size
 */
static size_t FindNalUnit(const uint8_t* data, size_t size, 
                          const uint8_t** start, size_t* nal_size, 
                          size_t offset = 0) {
    // 查找 start code (0x000001 或 0x00000001)
    size_t i = offset;
    while (i + 3 < size) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                // 0x000001
                *start = data + i + 3;
                break;
            } else if (data[i + 2] == 0 && i + 4 < size && data[i + 3] == 1) {
                // 0x00000001
                *start = data + i + 4;
                break;
            }
        }
        i++;
    }
    
    if (i + 3 >= size) {
        *start = nullptr;
        *nal_size = 0;
        return size;
    }
    
    // 查找下一个 start code 来确定 NAL 大小
    size_t nal_start = *start - data;
    size_t j = nal_start;
    while (j + 3 < size) {
        if (data[j] == 0 && data[j + 1] == 0 && 
            (data[j + 2] == 1 || (data[j + 2] == 0 && j + 4 < size && data[j + 3] == 1))) {
            *nal_size = j - nal_start;
            return j;
        }
        j++;
    }
    
    // 到末尾
    *nal_size = size - nal_start;
    return size;
}

// ============================================================================
// Mp4Recorder 实现
// ============================================================================

Mp4Recorder::Mp4Recorder(const Mp4RecordConfig& config)
    : config_(config) {
    EnsureDirectory(config_.outputDir);
    LOG_INFO("Mp4Recorder created, output dir: {}", config_.outputDir);
}

Mp4Recorder::~Mp4Recorder() {
    StopRecording();
    LOG_INFO("Mp4Recorder destroyed");
}

std::string Mp4Recorder::GenerateFilename() {
    return GenerateTimestampFilename(config_.filenamePrefix);
}

bool Mp4Recorder::CreateOutputFile(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    AVFormatContext* ofmt_ctx = nullptr;
    if (avformat_alloc_output_context2(&ofmt_ctx, nullptr, nullptr, filepath.c_str()) < 0) {
        LOG_ERROR("Could not create output context for: {}", filepath);
        return false;
    }
    
    AVCodecID codec_id = (config_.codecType == 12) ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    const AVCodec* codec = avcodec_find_encoder(codec_id);
    if (!codec) {
        LOG_ERROR("Codec not found: {}", (config_.codecType == 12) ? "H.265" : "H.264");
        avformat_free_context(ofmt_ctx);
        return false;
    }
    
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        LOG_ERROR("Failed to allocate codec context");
        avformat_free_context(ofmt_ctx);
        return false;
    }
    
    codec_ctx->codec_id = codec_id;
    codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    codec_ctx->bit_rate = 3000000;
    codec_ctx->width = config_.width;
    codec_ctx->height = config_.height;
    codec_ctx->time_base = AVRational{1, config_.fps};
    codec_ctx->framerate = AVRational{config_.fps, 1};
    codec_ctx->gop_size = config_.gopSize;
    codec_ctx->max_b_frames = 0;
    codec_ctx->pix_fmt = AV_PIX_FMT_NV12;
    
    AVStream* video_st = avformat_new_stream(ofmt_ctx, codec);
    if (!video_st) {
        LOG_ERROR("Failed to create video stream");
        avcodec_free_context(&codec_ctx);
        avformat_free_context(ofmt_ctx);
        return false;
    }
    
    if (avcodec_parameters_from_context(video_st->codecpar, codec_ctx) < 0) {
        LOG_ERROR("Failed to copy codec parameters to stream");
        avcodec_free_context(&codec_ctx);
        avformat_free_context(ofmt_ctx);
        return false;
    }
    
    video_st->codecpar->codec_tag = 0;
    video_st->time_base = AVRational{1, config_.fps};
    
    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ofmt_ctx->pb, filepath.c_str(), AVIO_FLAG_WRITE) < 0) {
            LOG_ERROR("Could not open output file: {}", filepath);
            avcodec_free_context(&codec_ctx);
            avformat_free_context(ofmt_ctx);
            return false;
        }
    }
    
    if (avformat_write_header(ofmt_ctx, nullptr) < 0) {
        LOG_ERROR("Error writing header to: {}", filepath);
        avio_closep(&ofmt_ctx->pb);
        avcodec_free_context(&codec_ctx);
        avformat_free_context(ofmt_ctx);
        return false;
    }
    
    format_ctx_ = ofmt_ctx;
    codec_ctx_ = codec_ctx;
    current_file_path_ = filepath;
    first_pts_ = 0;
    stats_ = Stats{};
    
    LOG_INFO("Created output file: {}", filepath);
    return true;
}

void Mp4Recorder::CloseOutputFile() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (format_ctx_) {
        AVFormatContext* ofmt_ctx = static_cast<AVFormatContext*>(format_ctx_);
        av_write_trailer(ofmt_ctx);
        
        if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&ofmt_ctx->pb);
        }
        
        avformat_free_context(ofmt_ctx);
        format_ctx_ = nullptr;
    }
    
    if (codec_ctx_) {
        AVCodecContext* ctx = static_cast<AVCodecContext*>(codec_ctx_);
        avcodec_free_context(&ctx);
        codec_ctx_ = nullptr;
    }
    
    if (!current_file_path_.empty()) {
        LOG_INFO("Closed output file: {}, {} frames, {:.2f} sec", 
                 current_file_path_, stats_.frames_written, stats_.duration_sec);
        current_file_path_.clear();
    }
    
    extradata_set_ = false;
}

bool Mp4Recorder::SetExtradataFromStream(const uint8_t* data, size_t size) {
    // 从 H.264 Annex B 流中提取 SPS 和 PPS
    // H.264 NAL 类型：7=SPS, 8=PPS
    
    const uint8_t* sps_data = nullptr;
    const uint8_t* pps_data = nullptr;
    size_t sps_size = 0;
    size_t pps_size = 0;
    
    size_t offset = 0;
    while (offset < size) {
        const uint8_t* nal_start = nullptr;
        size_t nal_size = 0;
        offset = FindNalUnit(data, size, &nal_start, &nal_size, offset);
        
        if (!nal_start || nal_size == 0) {
            break;
        }
        
        uint8_t nal_type = nal_start[0] & 0x1F;
        
        if (nal_type == 7 && !sps_data) {  // SPS
            sps_data = nal_start;
            sps_size = nal_size;
            LOG_DEBUG("Found SPS: {} bytes", sps_size);
        } else if (nal_type == 8 && !pps_data) {  // PPS
            pps_data = nal_start;
            pps_size = nal_size;
            LOG_DEBUG("Found PPS: {} bytes", pps_size);
        }
        
        if (sps_data && pps_data) {
            break;
        }
    }
    
    if (!sps_data || !pps_data) {
        LOG_WARN("SPS or PPS not found in stream");
        return false;
    }
    
    // 构造 AVCC 格式的 extradata
    // AVCC 格式：
    // 1 byte: version (1)
    // 1 byte: profile
    // 1 byte: compatibility
    // 1 byte: level
    // 1 byte: 0xFC | (lengthSizeMinusOne & 0x03)  -> 通常是 0xFF (4字节长度)
    // 1 byte: 0xE0 | numSPS  -> 通常是 0xE1 (1个SPS)
    // 2 bytes: SPS length (big endian)
    // SPS data
    // 1 byte: numPPS -> 通常是 0x01
    // 2 bytes: PPS length (big endian)
    // PPS data
    
    size_t extradata_size = 6 + 2 + sps_size + 1 + 2 + pps_size;
    uint8_t* extradata = static_cast<uint8_t*>(av_malloc(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!extradata) {
        LOG_ERROR("Failed to allocate extradata");
        return false;
    }
    memset(extradata, 0, extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    
    uint8_t* p = extradata;
    *p++ = 1;                          // version
    *p++ = sps_data[1];                // profile
    *p++ = sps_data[2];                // compatibility
    *p++ = sps_data[3];                // level
    *p++ = 0xFF;                       // 4 bytes NAL length
    *p++ = 0xE1;                       // 1 SPS
    *p++ = (sps_size >> 8) & 0xFF;     // SPS length high byte
    *p++ = sps_size & 0xFF;            // SPS length low byte
    memcpy(p, sps_data, sps_size);
    p += sps_size;
    *p++ = 1;                          // 1 PPS
    *p++ = (pps_size >> 8) & 0xFF;     // PPS length high byte
    *p++ = pps_size & 0xFF;            // PPS length low byte
    memcpy(p, pps_data, pps_size);
    
    // 设置到 codecpar
    AVFormatContext* ofmt_ctx = static_cast<AVFormatContext*>(format_ctx_);
    AVStream* video_stream = ofmt_ctx->streams[0];
    
    // 释放旧的 extradata（如果有）
    if (video_stream->codecpar->extradata) {
        av_free(video_stream->codecpar->extradata);
    }
    
    video_stream->codecpar->extradata = extradata;
    video_stream->codecpar->extradata_size = extradata_size;
    
    LOG_INFO("Set H.264 extradata: SPS={} bytes, PPS={} bytes", sps_size, pps_size);
    return true;
}

bool Mp4Recorder::StartRecording(const std::string& filename) {
    if (state_ != RecordState::kIdle) {
        LOG_WARN("Recording already in progress");
        return false;
    }
    
    std::string fname = filename.empty() ? GenerateFilename() : filename;
    std::string filepath = config_.outputDir + "/" + fname + ".mp4";
    
    if (!CreateOutputFile(filepath)) {
        return false;
    }
    
    state_ = RecordState::kRecording;
    LOG_INFO("Started recording to: {}", filepath);
    return true;
}

void Mp4Recorder::StopRecording() {
    if (state_ != RecordState::kRecording) {
        return;
    }
    
    state_ = RecordState::kStopping;
    CloseOutputFile();
    state_ = RecordState::kIdle;
    
    LOG_INFO("Stopped recording");
}

bool Mp4Recorder::WriteFrame(const EncodedStreamPtr& stream) {
    if (state_ != RecordState::kRecording || !stream || !stream->pstPack) {
        return false;
    }
    
    void* data = RK_MPI_MB_Handle2VirAddr(stream->pstPack->pMbBlk);
    if (!data || stream->pstPack->u32Len == 0) {
        LOG_WARN("Invalid frame data");
        return false;
    }
    
    bool is_keyframe = (stream->pstPack->DataType.enH264EType == H264E_NALU_IDRSLICE ||
                        stream->pstPack->DataType.enH264EType == H264E_NALU_ISLICE ||
                        stream->pstPack->DataType.enH265EType == H265E_NALU_IDRSLICE ||
                        stream->pstPack->DataType.enH265EType == H265E_NALU_ISLICE);
    
    // 在首个关键帧时提取 SPS/PPS 设置 extradata（不持有锁）
    if (!extradata_set_ && is_keyframe) {
        if (SetExtradataFromStream(static_cast<uint8_t*>(data), stream->pstPack->u32Len)) {
            extradata_set_ = true;
        } else {
            LOG_WARN("Failed to set extradata, MP4 may not play correctly");
        }
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!format_ctx_) {
        return false;
    }
    
    AVFormatContext* ofmt_ctx = static_cast<AVFormatContext*>(format_ctx_);
    AVStream* video_stream = ofmt_ctx->streams[0];
    
    int flags = 0;
    if (is_keyframe) {
        flags |= AV_PKT_FLAG_KEY;
    }
    
    if (first_pts_ == 0) {
        first_pts_ = stream->pstPack->u64PTS;
    }
    uint64_t relative_pts = (stream->pstPack->u64PTS - first_pts_) / 1000;
    
    AVPacket packet = {};
    packet.data = static_cast<uint8_t*>(data);
    packet.size = stream->pstPack->u32Len;
    packet.pts = av_rescale_q(relative_pts, AVRational{1, 1000}, video_stream->time_base);
    packet.dts = packet.pts;
    packet.stream_index = video_stream->index;
    packet.duration = av_rescale_q(1, AVRational{1, config_.fps}, video_stream->time_base);
    packet.flags = flags;
    
    int ret = av_interleaved_write_frame(ofmt_ctx, &packet);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Error writing frame: {}", errbuf);
        return false;
    }
    
    stats_.frames_written++;
    stats_.bytes_written += stream->pstPack->u32Len;
    stats_.duration_sec = static_cast<double>(relative_pts) / 1000.0;
    
    // 检查限制
    if (config_.maxDurationSec > 0 && stats_.duration_sec >= config_.maxDurationSec) {
        LOG_INFO("Max duration reached, stopping recording");
        // 注意：这里不能直接调用 StopRecording，因为会死锁
        // 应该在外部线程处理
    }
    
    return true;
}