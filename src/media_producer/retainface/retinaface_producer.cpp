/**
 * @file retinaface_producer.cpp
 * @brief RetinaFace 人脸检测模式生产者实现
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#define LOG_TAG "RetinaProd"

#include "retinaface_producer.h"
#include "mpi_config.h"
#include "retinaface_model.h"
#include "../common/image_utils.h"
#include "common/logger.h"
#include "common/media_buffer.h"
#include "common/asio_context.h"

#include "sample_comm.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_vpss.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_mb.h"

#include <cstring>
#include <chrono>

namespace media {

// ============================================================================
// 流分发器
// ============================================================================

class RetinaStreamDispatcher {
public:
    struct ConsumerInfo {
        std::string name;
        StreamCallback callback;
        StreamConsumerType type;
    };

    void RegisterConsumer(const std::string& name, StreamCallback callback,
                          StreamConsumerType type) {
        consumers_.push_back({name, std::move(callback), type});
        LOG_INFO("Registered stream consumer: {}", name);
    }

    void ClearConsumers() {
        consumers_.clear();
    }

    void DispatchFrame(EncodedStreamPtr stream) {
        for (auto& c : consumers_) {
            if (!c.callback) continue;
            if (c.type == StreamConsumerType::AsyncIO) {
                PostToIo([callback = c.callback, stream]() {
                    callback(stream);
                });
            } else {
                c.callback(stream);
            }
        }
    }

private:
    std::vector<ConsumerInfo> consumers_;
};

// ============================================================================
// MB Pool RAII 封装
// ============================================================================

class RetinaMbPool {
public:
    RetinaMbPool() = default;
    ~RetinaMbPool() { Destroy(); }

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
// RetinaFaceProducer 内部实现
// ============================================================================

struct RetinaFaceProducer::Impl {
    // MPI 状态
    bool isp_initialized = false;
    bool mpi_initialized = false;
    bool vi_enabled = false;
    bool vpss_enabled = false;
    bool venc_enabled = false;

    // 绑定句柄
    MPP_CHN_S vi_chn;
    MPP_CHN_S vpss_grp;

    // RGB 缓冲池
    RetinaMbPool rgb_pool;

    // AI 引擎
    std::unique_ptr<rknn::RetinaFaceModel> ai_model;
    std::unique_ptr<rknn::ImageProcessor> image_processor;
    
    // 中间缓冲区
    std::vector<uint8_t> temp_rgb_buffer;
    
    // 流分发器
    RetinaStreamDispatcher dispatcher;
};

// ============================================================================
// RetinaFaceProducer 实现
// ============================================================================

RetinaFaceProducer::RetinaFaceProducer(const ProducerConfig& config)
    : config_(config)
    , impl_(std::make_unique<Impl>()) {
    LOG_DEBUG("RetinaFaceProducer created");
}

RetinaFaceProducer::~RetinaFaceProducer() {
    Deinit();
    LOG_DEBUG("RetinaFaceProducer destroyed");
}

int RetinaFaceProducer::Init() {
    if (initialized_.load()) {
        LOG_WARN("Already initialized");
        return 0;
    }

    auto res = config_.GetResolutionConfig();
    LOG_INFO("Initializing RetinaFace producer: {}x{} @ {}fps, AI: {}x{}",
             res.width, res.height, res.framerate,
             config_.ai_width, config_.ai_height);

    if (InitMpi() != 0) {
        LOG_ERROR("Failed to initialize MPI");
        return -1;
    }

    if (!InitRgbPool()) {
        LOG_ERROR("Failed to initialize RGB pool");
        DeinitMpi();
        return -1;
    }

    if (InitAiEngine() != 0) {
        LOG_ERROR("Failed to initialize AI engine");
        DeinitMpi();
        return -1;
    }

    initialized_.store(true);
    LOG_INFO("RetinaFace producer initialized successfully");
    LOG_INFO("Pipeline: VI -> VPSS --(manual)--> NPU --(manual)--> VENC");
    return 0;
}

int RetinaFaceProducer::Deinit() {
    if (!initialized_.load()) {
        return 0;
    }

    Stop();
    DeinitAiEngine();
    impl_->rgb_pool.Destroy();
    DeinitMpi();

    initialized_.store(false);
    LOG_INFO("RetinaFace producer deinitialized");
    return 0;
}

bool RetinaFaceProducer::Start() {
    if (!initialized_.load()) {
        LOG_ERROR("Not initialized");
        return false;
    }

    if (running_.load()) {
        LOG_WARN("Already running");
        return true;
    }

    running_.store(true);
    frame_thread_ = std::thread(&RetinaFaceProducer::FrameLoop, this);
    LOG_INFO("RetinaFace producer started");
    return true;
}

void RetinaFaceProducer::Stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);
    if (frame_thread_.joinable()) {
        frame_thread_.join();
    }
    LOG_INFO("RetinaFace producer stopped");
}

void RetinaFaceProducer::RegisterStreamConsumer(
    const std::string& name,
    StreamCallback callback,
    StreamConsumerType type,
    int queue_size) {
    (void)queue_size;
    impl_->dispatcher.RegisterConsumer(name, std::move(callback), type);
}

void RetinaFaceProducer::ClearStreamConsumers() {
    impl_->dispatcher.ClearConsumers();
}

int RetinaFaceProducer::SetResolution(Resolution preset) {
    if (running_.load()) {
        LOG_WARN("Cannot change resolution while running");
        return -1;
    }
    config_.resolution = preset;
    return 0;
}

int RetinaFaceProducer::SetFrameRate(int fps) {
    if (running_.load()) {
        LOG_WARN("Cannot change framerate while running");
        return -1;
    }
    fps = std::max(1, std::min(fps, 30));
    config_.framerate = fps;
    return 0;
}

// ============================================================================
// MPI 初始化
// ============================================================================

int RetinaFaceProducer::InitMpi() {
    auto res = config_.GetResolutionConfig();
    RK_S32 ret;

    // 1. ISP 初始化
    const char* iq_dir = "/etc/iqfiles";
    SAMPLE_COMM_ISP_Init(kViDev, RK_AIQ_WORKING_MODE_NORMAL, RK_FALSE, iq_dir);
    SAMPLE_COMM_ISP_Run(kViDev);
    impl_->isp_initialized = true;
    LOG_DEBUG("ISP initialized");

    // 2. MPI 系统初始化
    ret = RK_MPI_SYS_Init();
    if (ret != RK_SUCCESS) {
        LOG_ERROR("RK_MPI_SYS_Init failed: {:#x}", ret);
        return -1;
    }
    impl_->mpi_initialized = true;
    LOG_DEBUG("MPI system initialized");

    // 3. VI 初始化
    vi_dev_init();
    vi_chn_init(kViChn, res.width, res.height);
    impl_->vi_enabled = true;
    LOG_DEBUG("VI initialized: {}x{}", res.width, res.height);

    // 4. VPSS 初始化（串行模式）
    ret = vpss_init_serial_mode(kVpssGrp, res.width, res.height);
    if (ret != 0) {
        LOG_ERROR("VPSS init failed");
        return -1;
    }
    impl_->vpss_enabled = true;
    LOG_DEBUG("VPSS initialized (serial mode)");

    // 5. VENC 初始化（RGB 输入）
    ret = venc_init_rgb_input(kVencChn, res.width, res.height, RK_VIDEO_ID_AVC);
    if (ret != 0) {
        LOG_ERROR("VENC init failed");
        return -1;
    }
    impl_->venc_enabled = true;
    LOG_DEBUG("VENC initialized (RGB input mode)");

    // 6. 绑定 VI -> VPSS
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
    LOG_DEBUG("VI -> VPSS bound");

    return 0;
}

int RetinaFaceProducer::DeinitMpi() {
    RK_MPI_SYS_UnBind(&impl_->vi_chn, &impl_->vpss_grp);

    if (impl_->venc_enabled) {
        RK_MPI_VENC_StopRecvFrame(kVencChn);

        VENC_STREAM_S stFrame;
        stFrame.pstPack = (VENC_PACK_S*)malloc(sizeof(VENC_PACK_S));
        if (stFrame.pstPack) {
            memset(stFrame.pstPack, 0, sizeof(VENC_PACK_S));
            int drain_count = 0;
            while (drain_count < 16) {
                RK_S32 ret = RK_MPI_VENC_GetStream(kVencChn, &stFrame, 50);
                if (ret != RK_SUCCESS) break;
                RK_MPI_VENC_ReleaseStream(kVencChn, &stFrame);
                drain_count++;
            }
            free(stFrame.pstPack);
        }

        RK_MPI_VENC_DestroyChn(kVencChn);
        impl_->venc_enabled = false;
    }

    if (impl_->vpss_enabled) {
        vpss_deinit(kVpssGrp, false);
        impl_->vpss_enabled = false;
    }

    if (impl_->vi_enabled) {
        RK_MPI_VI_DisableChn(kViDev, kViChn);
        RK_MPI_VI_DisableDev(kViDev);
        impl_->vi_enabled = false;
    }

    if (impl_->mpi_initialized) {
        RK_MPI_SYS_Exit();
        impl_->mpi_initialized = false;
    }

    if (impl_->isp_initialized) {
        SAMPLE_COMM_ISP_Stop(kViDev);
        impl_->isp_initialized = false;
    }

    return 0;
}

int RetinaFaceProducer::InitAiEngine() {
    // 初始化图像处理器
    impl_->image_processor = std::make_unique<rknn::ImageProcessor>();
    if (!impl_->image_processor->Init(config_.ai_width, config_.ai_height)) {
        LOG_ERROR("Failed to init image processor");
        return -1;
    }

    // 初始化 AI 模型
    impl_->ai_model = std::make_unique<rknn::RetinaFaceModel>();
    
    rknn::ModelConfig model_cfg;
    model_cfg.model_path = kDefaultModelPath;  // 使用 mpi_config.h 中的相对路径
    model_cfg.conf_threshold = 0.5f;
    model_cfg.nms_threshold = 0.4f;

    if (impl_->ai_model->Init(model_cfg) != 0) {
        LOG_ERROR("Failed to init RetinaFace model");
        return -1;
    }

    // 分配临时缓冲区
    auto res = config_.GetResolutionConfig();
    impl_->temp_rgb_buffer.resize(res.width * res.height * 3);

    LOG_INFO("AI engine initialized: RetinaFace {}x{}",
             config_.ai_width, config_.ai_height);
    return 0;
}

void RetinaFaceProducer::DeinitAiEngine() {
    if (impl_->ai_model) {
        impl_->ai_model->Deinit();
        impl_->ai_model.reset();
    }
    if (impl_->image_processor) {
        impl_->image_processor->Deinit();
        impl_->image_processor.reset();
    }
    impl_->temp_rgb_buffer.clear();
}

bool RetinaFaceProducer::InitRgbPool() {
    auto res = config_.GetResolutionConfig();
    size_t block_size = static_cast<size_t>(res.width) * res.height * 3;
    int block_count = 4;

    if (!impl_->rgb_pool.Create(block_size, block_count)) {
        return false;
    }

    LOG_DEBUG("RGB pool created: {} bytes x {} blocks", block_size, block_count);
    return true;
}

// ============================================================================
// 帧处理循环
// ============================================================================

void RetinaFaceProducer::FrameLoop() {
    LOG_INFO("RetinaFace frame loop started");

    auto res = config_.GetResolutionConfig();
    uint32_t time_ref = 0;
    VIDEO_FRAME_INFO_S vpss_frame;

    VIDEO_FRAME_INFO_S rgb_frame;
    memset(&rgb_frame, 0, sizeof(rgb_frame));
    rgb_frame.stVFrame.u32Width = res.width;
    rgb_frame.stVFrame.u32Height = res.height;
    rgb_frame.stVFrame.u32VirWidth = res.width;
    rgb_frame.stVFrame.u32VirHeight = res.height;
    rgb_frame.stVFrame.enPixelFormat = RK_FMT_RGB888;
    rgb_frame.stVFrame.u32FrameFlag = 160;

    while (running_.load()) {
        RK_S32 ret = RK_MPI_VPSS_GetChnFrame(kVpssGrp, kVpssChn0, &vpss_frame, 100);
        if (ret != RK_SUCCESS) {
            if (ret != RK_ERR_VPSS_BUF_EMPTY) {
                LOG_WARN("VPSS GetChnFrame failed: {:#x}", ret);
            }
            continue;
        }

        frame_count_++;

        void* vpss_data = RK_MPI_MB_Handle2VirAddr(vpss_frame.stVFrame.pMbBlk);
        if (!vpss_data) {
            RK_MPI_VPSS_ReleaseChnFrame(kVpssGrp, kVpssChn0, &vpss_frame);
            continue;
        }

        MB_BLK rgb_blk = impl_->rgb_pool.GetBlock(true);
        if (rgb_blk == MB_INVALID_HANDLE) {
            RK_MPI_VPSS_ReleaseChnFrame(kVpssGrp, kVpssChn0, &vpss_frame);
            continue;
        }

        void* rgb_data = RK_MPI_MB_Handle2VirAddr(rgb_blk);
        uint64_t pts = vpss_frame.stVFrame.u64PTS;

        bool processed = ProcessFrame(
            vpss_data,
            vpss_frame.stVFrame.u32Width,
            vpss_frame.stVFrame.u32Height,
            vpss_frame.stVFrame.u32VirWidth,
            rgb_data,
            pts
        );

        RK_MPI_VPSS_ReleaseChnFrame(kVpssGrp, kVpssChn0, &vpss_frame);

        if (!processed) {
            RetinaMbPool::ReleaseBlock(rgb_blk);
            continue;
        }

        rgb_frame.stVFrame.pMbBlk = rgb_blk;
        rgb_frame.stVFrame.u64PTS = pts;
        rgb_frame.stVFrame.u32TimeRef = time_ref++;

        ret = RK_MPI_VENC_SendFrame(kVencChn, &rgb_frame, 100);
        RetinaMbPool::ReleaseBlock(rgb_blk);

        if (ret != RK_SUCCESS) {
            continue;
        }

        RK_S32 last_error = 0;
        auto stream = acquire_encoded_stream(kVencChn, 100, &last_error);
        if (stream) {
            impl_->dispatcher.DispatchFrame(stream);
        }
    }

    LOG_INFO("RetinaFace frame loop exited, total frames: {}, inferences: {}",
             frame_count_.load(), inference_count_.load());
}

bool RetinaFaceProducer::ProcessFrame(void* nv12_data, int width, int height, int stride,
                                      void* rgb_output, uint64_t pts) {
    (void)pts;
    
    rknn::LetterboxInfo letterbox_info;
    int ret = impl_->image_processor->ConvertNV12ToModelInput(
        nv12_data, width, height, stride,
        impl_->ai_model->GetInputVirtAddr(),
        letterbox_info
    );

    if (ret != 0) {
        return false;
    }

    ret = impl_->ai_model->Run();
    if (ret != 0) {
        return false;
    }
    inference_count_++;

    rknn::DetectionResultList results;
    impl_->ai_model->GetResults(results);

    impl_->image_processor->ConvertNV12ToModelInput(
        nv12_data, width, height, stride,
        rgb_output,
        letterbox_info
    );

    impl_->image_processor->DrawDetections(
        rgb_output, width, height, results, letterbox_info
    );

    if (frame_count_ % 30 == 0 && results.Count() > 0) {
        LOG_DEBUG("Frame {}: {} faces", frame_count_.load(), results.Count());
    }

    return true;
}

// ============================================================================
// 工厂函数实现
// ============================================================================

std::unique_ptr<IMediaProducer> CreateRetinaFaceProducer(const ProducerConfig& config) {
    return std::make_unique<RetinaFaceProducer>(config);
}

}  // namespace media
