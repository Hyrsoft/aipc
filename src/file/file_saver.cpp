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
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!format_ctx_) {
        return false;
    }
    
    AVFormatContext* ofmt_ctx = static_cast<AVFormatContext*>(format_ctx_);
    AVStream* video_stream = ofmt_ctx->streams[0];
    
    void* data = RK_MPI_MB_Handle2VirAddr(stream->pstPack->pMbBlk);
    if (!data || stream->pstPack->u32Len == 0) {
        LOG_WARN("Invalid frame data");
        return false;
    }
    
    int flags = 0;
    bool is_keyframe = (stream->pstPack->DataType.enH264EType == H264E_NALU_IDRSLICE ||
                        stream->pstPack->DataType.enH264EType == H264E_NALU_ISLICE ||
                        stream->pstPack->DataType.enH265EType == H265E_NALU_IDRSLICE ||
                        stream->pstPack->DataType.enH265EType == H265E_NALU_ISLICE);
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

// ============================================================================
// JpegCapturer 实现
// ============================================================================

JpegCapturer::JpegCapturer(const JpegCaptureConfig& config)
    : config_(config) {
    
    if (!EnsureDirectory(config_.outputDir)) {
        LOG_ERROR("Failed to ensure output directory");
        return;
    }
    
    // 创建 JPEG VENC 通道
    VENC_CHN_ATTR_S enc_attr;
    memset(&enc_attr, 0, sizeof(enc_attr));
    
    enc_attr.stVencAttr.enType = RK_VIDEO_ID_JPEG;
    enc_attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    enc_attr.stVencAttr.u32MaxPicWidth = config_.width;
    enc_attr.stVencAttr.u32MaxPicHeight = config_.height;
    enc_attr.stVencAttr.u32PicWidth = config_.width;
    enc_attr.stVencAttr.u32PicHeight = config_.height;
    enc_attr.stVencAttr.u32VirWidth = config_.width;
    enc_attr.stVencAttr.u32VirHeight = config_.height;
    enc_attr.stVencAttr.u32StreamBufCnt = 2;
    enc_attr.stVencAttr.u32BufSize = config_.width * config_.height;
    
    RK_S32 ret = RK_MPI_VENC_CreateChn(config_.vencChnId, &enc_attr);
    if (ret != RK_SUCCESS) {
        LOG_ERROR("Failed to create JPEG VENC channel {}: 0x{:x}", config_.vencChnId, ret);
        return;
    }
    
    // 设置 JPEG 参数
    VENC_JPEG_PARAM_S jpeg_param;
    memset(&jpeg_param, 0, sizeof(jpeg_param));
    jpeg_param.u32Qfactor = config_.quality;
    RK_MPI_VENC_SetJpegParam(config_.vencChnId, &jpeg_param);
    
    // 设置旋转
    ROTATION_E rotation = ROTATION_0;
    if (config_.rotation == 90) rotation = ROTATION_90;
    else if (config_.rotation == 180) rotation = ROTATION_180;
    else if (config_.rotation == 270) rotation = ROTATION_270;
    RK_MPI_VENC_SetChnRotation(config_.vencChnId, rotation);
    
    valid_ = true;
    LOG_INFO("JpegCapturer created, VENC channel: {}, output dir: {}", 
             config_.vencChnId, config_.outputDir);
}

JpegCapturer::~JpegCapturer() {
    if (valid_) {
        RK_MPI_VENC_StopRecvFrame(config_.vencChnId);
        RK_MPI_VENC_DestroyChn(config_.vencChnId);
    }
    LOG_INFO("JpegCapturer destroyed, {} photos taken", completed_count_.load());
}

std::string JpegCapturer::GenerateFilename() {
    return GenerateTimestampFilename(config_.filenamePrefix);
}

bool JpegCapturer::TakeSnapshot(const std::string& filename) {
    if (!valid_) {
        LOG_ERROR("JpegCapturer not valid");
        return false;
    }
    
    if (pending_count_ > 0) {
        LOG_WARN("Previous snapshot still pending");
        return false;
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_filename_ = filename.empty() ? GenerateFilename() : filename;
    }
    
    // 触发 VENC 接收一帧
    VENC_RECV_PIC_PARAM_S recv_param;
    memset(&recv_param, 0, sizeof(recv_param));
    recv_param.s32RecvPicNum = 1;
    
    RK_S32 ret = RK_MPI_VENC_StartRecvFrame(config_.vencChnId, &recv_param);
    if (ret != RK_SUCCESS) {
        LOG_ERROR("Failed to start VENC recv frame: 0x{:x}", ret);
        return false;
    }
    
    pending_count_++;
    LOG_INFO("Snapshot triggered: {}", pending_filename_);
    return true;
}

bool JpegCapturer::SaveJpegData(const void* data, size_t len, const std::string& filepath) {
    FILE* fp = fopen(filepath.c_str(), "wb");
    if (!fp) {
        LOG_ERROR("Failed to open file for writing: {}", filepath);
        return false;
    }
    
    size_t written = fwrite(data, 1, len, fp);
    fclose(fp);
    
    if (written != len) {
        LOG_ERROR("Failed to write all data to file: {}", filepath);
        return false;
    }
    
    return true;
}

void JpegCapturer::ProcessLoop(std::atomic<bool>& running) {
    if (!valid_) {
        LOG_ERROR("JpegCapturer not valid, cannot start process loop");
        return;
    }
    
    LOG_DEBUG("JpegCapturer process loop started");
    
    // TDE 初始化
    RK_S32 ret = RK_TDE_Open();
    if (ret != RK_SUCCESS) {
        LOG_ERROR("Failed to open TDE: 0x{:x}", ret);
        return;
    }
    
    // 分配目标缓冲区
    PIC_BUF_ATTR_S dst_pic_buf_attr;
    MB_PIC_CAL_S dst_mb_pic_cal_result;
    MB_BLK dst_blk = nullptr;
    
    dst_pic_buf_attr.u32Width = config_.width;
    dst_pic_buf_attr.u32Height = config_.height;
    dst_pic_buf_attr.enPixelFormat = RK_FMT_YUV420SP;
    dst_pic_buf_attr.enCompMode = COMPRESS_MODE_NONE;
    
    ret = RK_MPI_CAL_TDE_GetPicBufferSize(&dst_pic_buf_attr, &dst_mb_pic_cal_result);
    if (ret != RK_SUCCESS) {
        LOG_ERROR("Failed to get TDE buffer size: 0x{:x}", ret);
        RK_TDE_Close();
        return;
    }
    
    ret = RK_MPI_SYS_MmzAlloc(&dst_blk, nullptr, nullptr, dst_mb_pic_cal_result.u32MBSize);
    if (ret != RK_SUCCESS) {
        LOG_ERROR("Failed to allocate TDE buffer: 0x{:x}", ret);
        RK_TDE_Close();
        return;
    }
    
    // VENC 输出帧结构
    VENC_STREAM_S frame;
    memset(&frame, 0, sizeof(frame));
    frame.pstPack = new VENC_PACK_S();
    memset(frame.pstPack, 0, sizeof(VENC_PACK_S));
    
    while (running) {
        if (pending_count_ <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // 从 VI 获取帧
        VIDEO_FRAME_INFO_S vi_frame;
        ret = RK_MPI_VI_GetChnFrame(config_.viDevId, config_.viChnId, &vi_frame, 1000);
        if (ret != RK_SUCCESS) {
            LOG_WARN("Failed to get VI frame: 0x{:x}", ret);
            continue;
        }
        
        // TDE 缩放处理
        TDE_HANDLE handle = RK_TDE_BeginJob();
        if (handle == RK_ERR_TDE_INVALID_HANDLE) {
            LOG_ERROR("Failed to begin TDE job");
            RK_MPI_VI_ReleaseChnFrame(config_.viDevId, config_.viChnId, &vi_frame);
            continue;
        }
        
        TDE_SURFACE_S src_surface, dst_surface;
        TDE_RECT_S src_rect, dst_rect;
        
        src_surface.pMbBlk = vi_frame.stVFrame.pMbBlk;
        src_surface.u32Width = vi_frame.stVFrame.u32Width;
        src_surface.u32Height = vi_frame.stVFrame.u32Height;
        src_surface.enColorFmt = RK_FMT_YUV420SP;
        src_surface.enComprocessMode = COMPRESS_MODE_NONE;
        
        src_rect.s32Xpos = 0;
        src_rect.s32Ypos = 0;
        src_rect.u32Width = vi_frame.stVFrame.u32Width;
        src_rect.u32Height = vi_frame.stVFrame.u32Height;
        
        dst_surface.pMbBlk = dst_blk;
        dst_surface.u32Width = config_.width;
        dst_surface.u32Height = config_.height;
        dst_surface.enColorFmt = RK_FMT_YUV420SP;
        dst_surface.enComprocessMode = COMPRESS_MODE_NONE;
        
        dst_rect.s32Xpos = 0;
        dst_rect.s32Ypos = 0;
        dst_rect.u32Width = config_.width;
        dst_rect.u32Height = config_.height;
        
        ret = RK_TDE_QuickResize(handle, &src_surface, &src_rect, &dst_surface, &dst_rect);
        if (ret != RK_SUCCESS) {
            LOG_ERROR("TDE resize failed: 0x{:x}", ret);
            RK_TDE_CancelJob(handle);
            RK_MPI_VI_ReleaseChnFrame(config_.viDevId, config_.viChnId, &vi_frame);
            continue;
        }
        
        ret = RK_TDE_EndJob(handle, RK_FALSE, RK_TRUE, 10);
        if (ret != RK_SUCCESS) {
            LOG_ERROR("TDE end job failed: 0x{:x}", ret);
            RK_TDE_CancelJob(handle);
            RK_MPI_VI_ReleaseChnFrame(config_.viDevId, config_.viChnId, &vi_frame);
            continue;
        }
        
        RK_TDE_WaitForDone(handle);
        
        // 更新 VENC 通道属性
        VENC_CHN_ATTR_S chn_attr;
        ret = RK_MPI_VENC_GetChnAttr(config_.vencChnId, &chn_attr);
        if (ret == RK_SUCCESS) {
            chn_attr.stVencAttr.u32PicWidth = config_.width;
            chn_attr.stVencAttr.u32PicHeight = config_.height;
            RK_MPI_VENC_SetChnAttr(config_.vencChnId, &chn_attr);
        }
        
        // 发送帧到 JPEG 编码器
        VIDEO_FRAME_INFO_S dst_frame;
        memset(&dst_frame, 0, sizeof(dst_frame));
        dst_frame.stVFrame.pMbBlk = dst_blk;
        dst_frame.stVFrame.u32Width = config_.width;
        dst_frame.stVFrame.u32Height = config_.height;
        dst_frame.stVFrame.u32VirWidth = config_.width;
        dst_frame.stVFrame.u32VirHeight = config_.height;
        dst_frame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
        dst_frame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
        
        ret = RK_MPI_VENC_SendFrame(config_.vencChnId, &dst_frame, 1000);
        if (ret != RK_SUCCESS) {
            LOG_ERROR("VENC send frame failed: 0x{:x}", ret);
            RK_MPI_VI_ReleaseChnFrame(config_.viDevId, config_.viChnId, &vi_frame);
            continue;
        }
        
        RK_MPI_VI_ReleaseChnFrame(config_.viDevId, config_.viChnId, &vi_frame);
        
        // 获取 JPEG 编码输出
        ret = RK_MPI_VENC_GetStream(config_.vencChnId, &frame, 1000);
        if (ret == RK_SUCCESS) {
            void* jpeg_data = RK_MPI_MB_Handle2VirAddr(frame.pstPack->pMbBlk);
            if (jpeg_data && frame.pstPack->u32Len > 0) {
                std::string filename;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    filename = pending_filename_;
                }
                std::string filepath = config_.outputDir + "/" + filename + ".jpeg";
                
                if (SaveJpegData(jpeg_data, frame.pstPack->u32Len, filepath)) {
                    last_photo_path_ = filepath;
                    completed_count_++;
                    LOG_INFO("Saved JPEG: {}, {} bytes", filepath, frame.pstPack->u32Len);
                }
            }
            
            RK_MPI_VENC_ReleaseStream(config_.vencChnId, &frame);
        } else {
            LOG_ERROR("Failed to get JPEG stream: 0x{:x}", ret);
        }
        
        pending_count_--;
    }
    
    // 清理
    delete frame.pstPack;
    RK_MPI_SYS_Free(dst_blk);
    RK_TDE_Close();
    
    LOG_DEBUG("JpegCapturer process loop exited");
}

