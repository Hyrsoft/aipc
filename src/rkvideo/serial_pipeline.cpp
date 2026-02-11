/**
 * @file serial_pipeline.cpp
 * @brief 串行管道处理器实现
 *
 * @author 好软，好温暖
 * @date 2026-02-11
 */

#define LOG_TAG "SerialPipe"

#include "serial_pipeline.h"
#include "luckfox_mpi.h"
#include "common/logger.h"

#include <cstring>

namespace rkvideo {

// ============================================================================
// MPI 通道/Group 定义
// ============================================================================

static constexpr int kViDev = 0;
static constexpr int kViChn = 0;
static constexpr int kVpssGrp = 0;
static constexpr int kVpssChn0 = 0;  // 全分辨率给 VENC（串行模式：手动 SendFrame）
static constexpr int kVpssChn1 = 1;  // 可选的 AI 缩放通道
static constexpr int kVencChn = 0;

// ============================================================================
// 辅助类 - RGB 缓冲池
// ============================================================================

/**
 * @brief 简化的 MbPool RAII 封装
 */
class MbPool {
public:
    MbPool() = default;
    ~MbPool() { Destroy(); }

    bool Create(size_t block_size, int block_count) {
        MB_POOL_CONFIG_S cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.u64MBSize = block_size;
        cfg.u32MBCnt = block_count;
        cfg.enAllocType = MB_ALLOC_TYPE_DMA;

        pool_ = RK_MPI_MB_CreatePool(&cfg);
        if (pool_ == MB_INVALID_POOLID) {
            LOG_ERROR("Failed to create MB pool");
            return false;
        }
        block_size_ = block_size;
        return true;
    }

    void Destroy() {
        if (pool_ != MB_INVALID_POOLID) {
            RK_MPI_MB_DestroyPool(pool_);
            pool_ = MB_INVALID_POOLID;
        }
    }

    MB_BLK GetBlock(bool blocking = true) {
        if (pool_ == MB_INVALID_POOLID) return MB_INVALID_HANDLE;
        return RK_MPI_MB_GetMB(pool_, block_size_, blocking ? RK_TRUE : RK_FALSE);
    }

    static void ReleaseBlock(MB_BLK blk) {
        if (blk != MB_INVALID_HANDLE) {
            RK_MPI_MB_ReleaseMB(blk);
        }
    }

    MB_POOL Handle() const { return pool_; }
    size_t BlockSize() const { return block_size_; }

private:
    MB_POOL pool_ = MB_INVALID_POOLID;
    size_t block_size_ = 0;
};

// ============================================================================
// SerialPipeline 实现
// ============================================================================

// 内部实现类定义
struct SerialPipeline::Impl {
    MbPool rgb_pool;
    bool mpi_initialized = false;
    bool vi_enabled = false;
    bool vpss_enabled = false;
    bool venc_enabled = false;
    
    // 绑定状态
    MPP_CHN_S vi_chn;
    MPP_CHN_S vpss_grp;  // 作为绑定目标使用
};

SerialPipeline::SerialPipeline() 
    : impl_(std::make_unique<Impl>()) {
}

SerialPipeline::~SerialPipeline() {
    Stop();
    Deinit();
}

int SerialPipeline::Init(const SerialPipelineConfig& config) {
    if (initialized_.load()) {
        LOG_WARN("Serial pipeline already initialized");
        return 0;
    }

    config_ = config;
    
    if (InitMpi() != 0) {
        return -1;
    }
    
    if (!InitRgbPool()) {
        DeinitMpi();
        return -1;
    }
    
    // 创建流分发器
    dispatcher_ = std::make_unique<StreamDispatcher>(kVencChn);
    
    initialized_.store(true);
    LOG_INFO("Serial pipeline initialized: {}x{} @ {}fps", 
             config_.width, config_.height, config_.frame_rate);
    return 0;
}

int SerialPipeline::Deinit() {
    if (!initialized_.load()) {
        return 0;
    }
    
    Stop();
    
    if (dispatcher_) {
        dispatcher_->Stop();
        dispatcher_.reset();
    }
    
    if (impl_) {
        impl_->rgb_pool.Destroy();
    }
    
    DeinitMpi();
    
    initialized_.store(false);
    LOG_INFO("Serial pipeline deinitialized");
    return 0;
}

bool SerialPipeline::InitRgbPool() {
    if (!impl_) return false;
    
    // RGB888 格式，每像素 3 字节
    size_t block_size = static_cast<size_t>(config_.width) * config_.height * 3;
    int block_count = 4;
    
    if (!impl_->rgb_pool.Create(block_size, block_count)) {
        LOG_ERROR("Failed to create RGB buffer pool");
        return false;
    }
    
    LOG_DEBUG("RGB pool created: {} bytes x {} blocks", block_size, block_count);
    return true;
}

int SerialPipeline::InitMpi() {
    if (!impl_) return -1;
    RK_S32 ret;
    
    LOG_INFO("Initializing serial mode MPI pipeline...");
    
    // ==================== ISP 初始化 ====================
    const char* iq_dir = "/etc/iqfiles";
    SAMPLE_COMM_ISP_Init(0, RK_AIQ_WORKING_MODE_NORMAL, RK_FALSE, iq_dir);
    SAMPLE_COMM_ISP_Run(0);
    LOG_DEBUG("ISP initialized");
    
    // ==================== MPI 系统初始化 ====================
    ret = RK_MPI_SYS_Init();
    if (ret != RK_SUCCESS) {
        LOG_ERROR("RK_MPI_SYS_Init failed: {:#x}", ret);
        return -1;
    }
    impl_->mpi_initialized = true;
    LOG_DEBUG("MPI system initialized");
    
    // ==================== VI 初始化 ====================
    vi_dev_init();
    vi_chn_init(kViChn, config_.width, config_.height);
    impl_->vi_enabled = true;
    LOG_DEBUG("VI initialized: {}x{}", config_.width, config_.height);
    
    // ==================== VPSS 初始化 ====================
    // 串行模式：Chn0 的 u32Depth > 0，允许手动 GetFrame
    // 不使用 Chn1（全部在 Chn0 处理）
    ret = vpss_init_serial_mode(kVpssGrp, config_.width, config_.height);
    if (ret != 0) {
        LOG_ERROR("VPSS init failed!");
        return -1;
    }
    impl_->vpss_enabled = true;
    LOG_DEBUG("VPSS initialized (serial mode)");
    
    // ==================== VENC 初始化 ====================
    // 串行模式：使用 RGB888 输入（用于 OSD 叠加后的帧）
    ret = venc_init_rgb_input(kVencChn, config_.width, config_.height, RK_VIDEO_ID_AVC);
    if (ret != 0) {
        LOG_ERROR("VENC init failed!");
        return -1;
    }
    impl_->venc_enabled = true;
    LOG_DEBUG("VENC initialized (RGB input mode)");
    
    // ==================== 绑定：VI -> VPSS ====================
    // 只绑定 VI 到 VPSS，不绑定 VPSS 到 VENC（串行模式）
    impl_->vi_chn.enModId = RK_ID_VI;
    impl_->vi_chn.s32DevId = kViDev;
    impl_->vi_chn.s32ChnId = kViChn;
    
    impl_->vpss_grp.enModId = RK_ID_VPSS;
    impl_->vpss_grp.s32DevId = kVpssGrp;
    impl_->vpss_grp.s32ChnId = 0;
    
    ret = RK_MPI_SYS_Bind(&impl_->vi_chn, &impl_->vpss_grp);
    if (ret != RK_SUCCESS) {
        LOG_ERROR("Failed to bind VI -> VPSS: {:#x}", ret);
        return -1;
    }
    LOG_DEBUG("VI -> VPSS binding successful (VPSS -> VENC NOT bound)");
    
    LOG_INFO("Serial mode pipeline: VI -> VPSS --(manual)--> [Process] --(manual)--> VENC");
    return 0;
}

int SerialPipeline::DeinitMpi() {
    if (!impl_) return 0;
    
    LOG_INFO("Deinitializing serial mode MPI pipeline...");
    
    // 解除绑定
    RK_MPI_SYS_UnBind(&impl_->vi_chn, &impl_->vpss_grp);
    
    // 停止 VENC
    if (impl_->venc_enabled) {
        RK_MPI_VENC_StopRecvFrame(kVencChn);
        
        // 排空 VENC
        VENC_STREAM_S stFrame;
        stFrame.pstPack = (VENC_PACK_S*)malloc(sizeof(VENC_PACK_S));
        if (stFrame.pstPack) {
            memset(stFrame.pstPack, 0, sizeof(VENC_PACK_S));
            int drainCount = 0;
            while (drainCount < 16) {
                RK_S32 ret = RK_MPI_VENC_GetStream(kVencChn, &stFrame, 50);
                if (ret != RK_SUCCESS) break;
                RK_MPI_VENC_ReleaseStream(kVencChn, &stFrame);
                drainCount++;
            }
            free(stFrame.pstPack);
        }
        
        RK_MPI_VENC_DestroyChn(kVencChn);
        impl_->venc_enabled = false;
    }
    
    // 销毁 VPSS
    if (impl_->vpss_enabled) {
        vpss_deinit(kVpssGrp, false);
        impl_->vpss_enabled = false;
    }
    
    // 销毁 VI
    if (impl_->vi_enabled) {
        RK_MPI_VI_DisableChn(kViDev, kViChn);
        RK_MPI_VI_DisableDev(kViDev);
        impl_->vi_enabled = false;
    }
    
    // MPI 系统退出
    if (impl_->mpi_initialized) {
        RK_MPI_SYS_Exit();
        impl_->mpi_initialized = false;
    }
    
    // 停止 ISP
    SAMPLE_COMM_ISP_Stop(0);
    
    LOG_INFO("Serial mode MPI pipeline deinitialized");
    return 0;
}

bool SerialPipeline::Start(FrameProcessCallback process_callback) {
    if (!initialized_.load()) {
        LOG_ERROR("Pipeline not initialized");
        return false;
    }
    
    if (running_.load()) {
        LOG_WARN("Pipeline already running");
        return true;
    }
    
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        process_callback_ = std::move(process_callback);
    }
    
    running_.store(true);
    frame_thread_ = std::thread(&SerialPipeline::FrameLoop, this);
    
    if (dispatcher_) {
        dispatcher_->Start();
    }
    
    LOG_INFO("Serial pipeline started");
    return true;
}

void SerialPipeline::Stop() {
    if (!running_.load()) {
        return;
    }
    
    running_.store(false);
    
    if (frame_thread_.joinable()) {
        frame_thread_.join();
    }
    
    if (dispatcher_) {
        dispatcher_->Stop();
    }
    
    LOG_INFO("Serial pipeline stopped");
}

void SerialPipeline::RegisterStreamConsumer(const char* name,
                                             std::function<void(EncodedStreamPtr)> callback,
                                             ConsumerType type,
                                             int queue_size) {
    if (dispatcher_) {
        dispatcher_->RegisterConsumer(name, std::move(callback), type, queue_size);
        LOG_DEBUG("Registered stream consumer: {}", name);
    }
}

void SerialPipeline::FrameLoop() {
    LOG_INFO("Serial frame loop started");
    
    if (!impl_) {
        LOG_ERROR("impl_ is null in FrameLoop");
        return;
    }
    
    uint32_t time_ref = 0;
    VIDEO_FRAME_INFO_S vpss_frame;
    
    // VENC 帧结构（RGB 输入）
    VIDEO_FRAME_INFO_S rgb_frame;
    memset(&rgb_frame, 0, sizeof(rgb_frame));
    rgb_frame.stVFrame.u32Width = config_.width;
    rgb_frame.stVFrame.u32Height = config_.height;
    rgb_frame.stVFrame.u32VirWidth = config_.width;
    rgb_frame.stVFrame.u32VirHeight = config_.height;
    rgb_frame.stVFrame.enPixelFormat = RK_FMT_RGB888;
    rgb_frame.stVFrame.u32FrameFlag = 160;
    
    while (running_.load()) {
        // 1. 从 VPSS Chn0 获取帧（串行模式，Depth > 0）
        RK_S32 ret = RK_MPI_VPSS_GetChnFrame(kVpssGrp, kVpssChn0, &vpss_frame, 
                                              config_.frame_timeout_ms);
        if (ret != RK_SUCCESS) {
            if (ret != RK_ERR_VPSS_BUF_EMPTY) {
                LOG_WARN("VPSS GetChnFrame failed: {:#x}", ret);
            }
            continue;
        }
        
        frame_count_++;
        
        // 获取帧数据地址
        void* vpss_data = RK_MPI_MB_Handle2VirAddr(vpss_frame.stVFrame.pMbBlk);
        if (!vpss_data) {
            RK_MPI_VPSS_ReleaseChnFrame(kVpssGrp, kVpssChn0, &vpss_frame);
            continue;
        }
        
        // 2. 获取 RGB buffer
        MB_BLK rgb_blk = impl_->rgb_pool.GetBlock(true);
        if (rgb_blk == MB_INVALID_HANDLE) {
            RK_MPI_VPSS_ReleaseChnFrame(kVpssGrp, kVpssChn0, &vpss_frame);
            continue;
        }
        
        void* rgb_data = RK_MPI_MB_Handle2VirAddr(rgb_blk);
        if (!rgb_data) {
            MbPool::ReleaseBlock(rgb_blk);
            RK_MPI_VPSS_ReleaseChnFrame(kVpssGrp, kVpssChn0, &vpss_frame);
            continue;
        }
        
        // 3. 调用用户回调进行处理（NV12 -> RGB, AI 推理, OSD 叠加）
        bool frame_processed = false;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (process_callback_) {
                RawFrame raw;
                raw.data = vpss_data;
                raw.width = vpss_frame.stVFrame.u32Width;
                raw.height = vpss_frame.stVFrame.u32Height;
                raw.stride = vpss_frame.stVFrame.u32VirWidth;
                raw.pts = vpss_frame.stVFrame.u64PTS;
                raw.mpi_handle = vpss_frame.stVFrame.pMbBlk;
                
                ProcessedFrame processed;
                processed.data = rgb_data;
                processed.width = config_.width;
                processed.height = config_.height;
                processed.mb_handle = rgb_blk;
                
                frame_processed = process_callback_(raw, processed);
                
                if (frame_processed) {
                    processed.pts = raw.pts;  // 保持时间戳
                }
            }
        }
        
        // 释放 VPSS 帧（必须在处理后释放）
        RK_MPI_VPSS_ReleaseChnFrame(kVpssGrp, kVpssChn0, &vpss_frame);
        
        if (!frame_processed) {
            MbPool::ReleaseBlock(rgb_blk);
            continue;
        }
        
        // 4. 手动发送帧到 VENC
        rgb_frame.stVFrame.pMbBlk = rgb_blk;
        rgb_frame.stVFrame.u32TimeRef = time_ref++;
        rgb_frame.stVFrame.u64PTS = TEST_COMM_GetNowUs();
        
        ret = RK_MPI_VENC_SendFrame(kVencChn, &rgb_frame, 1000);
        if (ret != RK_SUCCESS) {
            LOG_WARN("VENC SendFrame failed: {:#x}", ret);
            MbPool::ReleaseBlock(rgb_blk);
            continue;
        }
        
        // 释放 RGB buffer（VENC 已经拷贝）
        MbPool::ReleaseBlock(rgb_blk);
        encode_count_++;
        
        // 注意：不在这里调用 GetStream，由 StreamDispatcher 的 FetchLoop 负责
        // 这避免了两个线程竞争 VENC 输出
    }
    
    LOG_INFO("Serial frame loop stopped, frames: {}, encoded: {}", 
             frame_count_.load(), encode_count_.load());
}

}  // namespace rkvideo

// ============================================================================
// 串行模式专用的 MPI 初始化函数
// ============================================================================

/**
 * @brief 串行模式 VPSS 初始化
 * 
 * 与并行模式的区别：
 * - Chn0 的 u32Depth > 0，允许手动 GetChnFrame
 * - 不绑定到 VENC
 */
int vpss_init_serial_mode(int grpId, int width, int height) {
    printf("%s: grp=%d, size=%dx%d (serial mode)\n", __func__, grpId, width, height);
    
    int ret;
    
    // Group 配置
    VPSS_GRP_ATTR_S stVpssGrpAttr;
    memset(&stVpssGrpAttr, 0, sizeof(VPSS_GRP_ATTR_S));
    stVpssGrpAttr.u32MaxW = width;
    stVpssGrpAttr.u32MaxH = height;
    stVpssGrpAttr.enPixelFormat = RK_FMT_YUV420SP;
    stVpssGrpAttr.enCompressMode = COMPRESS_MODE_NONE;
    stVpssGrpAttr.stFrameRate.s32SrcFrameRate = -1;
    stVpssGrpAttr.stFrameRate.s32DstFrameRate = -1;
    
    ret = RK_MPI_VPSS_CreateGrp(grpId, &stVpssGrpAttr);
    if (ret != RK_SUCCESS) {
        printf("ERROR: RK_MPI_VPSS_CreateGrp failed! ret=0x%x\n", ret);
        return -1;
    }
    
    // Chn0 配置（串行模式：u32Depth = 2）
    VPSS_CHN_ATTR_S stVpssChn0Attr;
    memset(&stVpssChn0Attr, 0, sizeof(VPSS_CHN_ATTR_S));
    stVpssChn0Attr.enChnMode = VPSS_CHN_MODE_USER;
    stVpssChn0Attr.enCompressMode = COMPRESS_MODE_NONE;
    stVpssChn0Attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
    stVpssChn0Attr.enPixelFormat = RK_FMT_YUV420SP;
    stVpssChn0Attr.stFrameRate.s32SrcFrameRate = -1;
    stVpssChn0Attr.stFrameRate.s32DstFrameRate = -1;
    stVpssChn0Attr.u32Width = width;
    stVpssChn0Attr.u32Height = height;
    stVpssChn0Attr.u32Depth = 2;  // 串行模式：允许 GetChnFrame
    stVpssChn0Attr.bFlip = RK_FALSE;
    stVpssChn0Attr.bMirror = RK_FALSE;
    stVpssChn0Attr.u32FrameBufCnt = 4;
    
    ret = RK_MPI_VPSS_SetChnAttr(grpId, VPSS_CHN0, &stVpssChn0Attr);
    if (ret != RK_SUCCESS) {
        printf("ERROR: RK_MPI_VPSS_SetChnAttr Chn0 failed! ret=0x%x\n", ret);
        RK_MPI_VPSS_DestroyGrp(grpId);
        return -1;
    }
    
    ret = RK_MPI_VPSS_EnableChn(grpId, VPSS_CHN0);
    if (ret != RK_SUCCESS) {
        printf("ERROR: RK_MPI_VPSS_EnableChn Chn0 failed! ret=0x%x\n", ret);
        RK_MPI_VPSS_DestroyGrp(grpId);
        return -1;
    }
    
    ret = RK_MPI_VPSS_StartGrp(grpId);
    if (ret != RK_SUCCESS) {
        printf("ERROR: RK_MPI_VPSS_StartGrp failed! ret=0x%x\n", ret);
        RK_MPI_VPSS_DisableChn(grpId, VPSS_CHN0);
        RK_MPI_VPSS_DestroyGrp(grpId);
        return -1;
    }
    
    printf("VPSS init (serial mode) success!\n");
    return 0;
}

/**
 * @brief RGB 输入模式的 VENC 初始化
 * 
 * 与并行模式的区别：
 * - 输入像素格式为 RGB888（用于接收 OSD 叠加后的帧）
 */
int venc_init_rgb_input(int chnId, int width, int height, RK_CODEC_ID_E enType) {
    printf("%s: chn=%d, size=%dx%d (RGB input)\n", __func__, chnId, width, height);
    
    VENC_CHN_ATTR_S stAttr;
    memset(&stAttr, 0, sizeof(VENC_CHN_ATTR_S));
    
    if (enType == RK_VIDEO_ID_AVC) {
        stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
        stAttr.stRcAttr.stH264Cbr.u32BitRate = 10 * 1024;
        stAttr.stRcAttr.stH264Cbr.u32Gop = 60;
    } else if (enType == RK_VIDEO_ID_HEVC) {
        stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
        stAttr.stRcAttr.stH265Cbr.u32BitRate = 10 * 1024;
        stAttr.stRcAttr.stH265Cbr.u32Gop = 60;
    }
    
    stAttr.stVencAttr.enType = enType;
    stAttr.stVencAttr.enPixelFormat = RK_FMT_RGB888;  // RGB 输入模式
    if (enType == RK_VIDEO_ID_AVC) {
        stAttr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
    }
    stAttr.stVencAttr.u32PicWidth = width;
    stAttr.stVencAttr.u32PicHeight = height;
    stAttr.stVencAttr.u32VirWidth = width;
    stAttr.stVencAttr.u32VirHeight = height;
    stAttr.stVencAttr.u32StreamBufCnt = 4;
    stAttr.stVencAttr.u32BufSize = width * height * 3;  // RGB888
    stAttr.stVencAttr.enMirror = MIRROR_NONE;
    
    RK_S32 ret = RK_MPI_VENC_CreateChn(chnId, &stAttr);
    if (ret != RK_SUCCESS) {
        printf("ERROR: RK_MPI_VENC_CreateChn failed! ret=0x%x\n", ret);
        return -1;
    }
    
    VENC_RECV_PIC_PARAM_S stRecvParam;
    memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
    stRecvParam.s32RecvPicNum = -1;
    RK_MPI_VENC_StartRecvFrame(chnId, &stRecvParam);
    
    printf("VENC init (RGB input) success!\n");
    return 0;
}
