/**
 * @file visiong_producer.cpp
 * @brief VisionG 库驱动的 AI 推理模式生产者实现
 *
 * 使用 VisionG Camera 取帧 + NPU 推理 + ImageBuffer OSD，
 * VENC 编码仍使用 RKMPI 接口以保持 EncodedStreamPtr 兼容性。
 *
 * @author AI Assistant
 * @date 2026-03-20
 */

#define LOG_TAG "VisionGProd"

#include "visiong_producer.h"
#include "common/asio_context.h"
#include "common/logger.h"
#include "common/media_buffer.h"

// VisionG 库头文件
#include "visiong/core/Camera.h"
#include "visiong/core/ImageBuffer.h"
#include "visiong/npu/NPU.h"

// RKMPI 头文件（VENC 编码）
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "sample_comm.h"

#include <cstring>
#include <ctime>

namespace media {

// ============================================================================
// VENC 通道常量
// ============================================================================

static constexpr int kVencChn = 0;

// ============================================================================
// 流分发器（与 SimpleIPCProducer / YoloProducer 共用逻辑）
// ============================================================================

class VisionGStreamDispatcher {
public:
    struct ConsumerInfo {
        std::string name;
        StreamCallback callback;
        StreamConsumerType type;
    };

    void RegisterConsumer(const std::string& name, StreamCallback callback, StreamConsumerType type) {
        consumers_.push_back({name, std::move(callback), type});
        LOG_INFO("Registered stream consumer: {}", name);
    }

    void ClearConsumers() { consumers_.clear(); }

    void DispatchFrame(EncodedStreamPtr stream) {
        for (auto& c : consumers_) {
            if (!c.callback)
                continue;
            if (c.type == StreamConsumerType::AsyncIO) {
                PostToIo([callback = c.callback, stream]() { callback(stream); });
            } else {
                c.callback(stream);
            }
        }
    }

private:
    std::vector<ConsumerInfo> consumers_;
};

// ============================================================================
// VisionGProducer 内部实现
// ============================================================================

struct VisionGProducer::Impl {
    // VENC 编码用的 MB Pool
    MB_POOL rgb_pool = MB_INVALID_POOLID;
    MB_BLK rgb_blk = MB_INVALID_HANDLE;
    size_t rgb_block_size = 0;

    // 流分发器
    VisionGStreamDispatcher dispatcher;
};

// ============================================================================
// 辅助函数
// ============================================================================

static RK_U64 GetNowUs() {
    struct timespec time = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (RK_U64)time.tv_sec * 1000000 + (RK_U64)time.tv_nsec / 1000;
}

/**
 * @brief 强制清理 VENC 残留状态
 */
static void ForceCleanupVenc() {
    RK_MPI_VENC_StopRecvFrame(kVencChn);
    RK_MPI_VENC_DestroyChn(kVencChn);
}

/**
 * @brief 初始化 VENC（RGB888 输入，与 yolo_producer 的 venc_init 一致）
 */
static int InitVencChannel(int width, int height) {
    VENC_CHN_ATTR_S stAttr;
    memset(&stAttr, 0, sizeof(VENC_CHN_ATTR_S));

    stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    stAttr.stRcAttr.stH264Cbr.u32BitRate = 10 * 1024;
    stAttr.stRcAttr.stH264Cbr.u32Gop = 1;

    stAttr.stVencAttr.enType = RK_VIDEO_ID_AVC;
    stAttr.stVencAttr.enPixelFormat = RK_FMT_RGB888;
    stAttr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
    stAttr.stVencAttr.u32PicWidth = width;
    stAttr.stVencAttr.u32PicHeight = height;
    stAttr.stVencAttr.u32VirWidth = width;
    stAttr.stVencAttr.u32VirHeight = height;
    stAttr.stVencAttr.u32StreamBufCnt = 2;
    stAttr.stVencAttr.u32BufSize = width * height * 3 / 2;
    stAttr.stVencAttr.enMirror = MIRROR_NONE;

    RK_S32 ret = RK_MPI_VENC_CreateChn(kVencChn, &stAttr);
    if (ret != RK_SUCCESS) {
        LOG_ERROR("VENC CreateChn failed: {:#x}", ret);
        return -1;
    }

    VENC_RECV_PIC_PARAM_S stRecvParam;
    memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
    stRecvParam.s32RecvPicNum = -1;
    RK_MPI_VENC_StartRecvFrame(kVencChn, &stRecvParam);

    return 0;
}

// ============================================================================
// VisionGProducer 实现
// ============================================================================

VisionGProducer::VisionGProducer(const ProducerConfig& config,
                                 ModelType model_type,
                                 const std::string& model_path,
                                 const std::string& label_path)
    : config_(config),
      model_type_(model_type),
      model_path_(model_path),
      label_path_(label_path),
      impl_(std::make_unique<Impl>()) {

    // 根据模型类型设置类型名称
    switch (model_type_) {
        case ModelType::YOLOV5:
            type_name_ = "VisionG_YoloV5";
            break;
        case ModelType::RETINAFACE:
            type_name_ = "VisionG_RetinaFace";
            break;
        default:
            type_name_ = "VisionG_Unknown";
            break;
    }

    LOG_DEBUG("VisionGProducer created ({})", type_name_);
}

VisionGProducer::~VisionGProducer() {
    Deinit();
    LOG_DEBUG("VisionGProducer destroyed ({})", type_name_);
}

int VisionGProducer::Init() {
    if (initialized_.load()) {
        LOG_WARN("Already initialized");
        return 0;
    }

    auto res = config_.GetResolutionConfig();
    LOG_INFO("Initializing VisionG producer ({}): {}x{} @ {}fps",
             type_name_, res.width, res.height, res.framerate);

    // 1. 初始化 MPI 系统
    RK_S32 ret = RK_MPI_SYS_Init();
    if (ret != RK_SUCCESS) {
        LOG_ERROR("RK_MPI_SYS_Init failed: {:#x}", ret);
        return -1;
    }

    // 2. 初始化 VisionG Camera
    camera_ = std::make_unique<Camera>();
    if (!camera_->init(res.width, res.height, "yuv")) {
        LOG_ERROR("VisionG Camera init failed");
        camera_.reset();
        RK_MPI_SYS_Exit();
        return -1;
    }
    LOG_INFO("VisionG Camera initialized: {}x{}", 
             camera_->actual_width(), camera_->actual_height());

    // 跳过前几帧（自动曝光稳定）
    camera_->skip(5);

    // 3. 初始化 VisionG NPU
    npu_ = std::make_unique<NPU>(
        model_type_,
        model_path_,
        label_path_,
        0.25f,  // box_thresh
        0.45f   // nms_thresh
    );
    if (!npu_->is_initialized()) {
        LOG_ERROR("VisionG NPU init failed: model={}", model_path_);
        camera_->release();
        camera_.reset();
        RK_MPI_SYS_Exit();
        return -1;
    }
    LOG_INFO("VisionG NPU initialized: model={}x{}", 
             npu_->model_width(), npu_->model_height());

    // 4. 初始化 VENC
    ForceCleanupVenc(); // 清理残留
    if (InitVenc() != 0) {
        LOG_ERROR("VENC init failed");
        npu_.reset();
        camera_->release();
        camera_.reset();
        RK_MPI_SYS_Exit();
        return -1;
    }

    initialized_.store(true);
    LOG_INFO("VisionG producer ({}) initialized successfully", type_name_);
    LOG_INFO("Pipeline: Camera::snapshot() → NPU::inference() → OSD → VENC");
    return 0;
}

int VisionGProducer::Deinit() {
    if (!initialized_.load()) {
        return 0;
    }

    Stop();

    // 释放 MB Pool
    if (impl_->rgb_blk != MB_INVALID_HANDLE) {
        RK_MPI_MB_ReleaseMB(impl_->rgb_blk);
        impl_->rgb_blk = MB_INVALID_HANDLE;
    }
    if (impl_->rgb_pool != MB_INVALID_POOLID) {
        RK_MPI_MB_DestroyPool(impl_->rgb_pool);
        impl_->rgb_pool = MB_INVALID_POOLID;
    }

    // 释放 VENC
    DeinitVenc();

    // 释放 VisionG NPU
    npu_.reset();

    // 释放 VisionG Camera
    if (camera_) {
        camera_->release();
        camera_.reset();
    }

    // 释放 MPI 系统
    RK_MPI_SYS_Exit();

    initialized_.store(false);
    LOG_INFO("VisionG producer ({}) deinitialized", type_name_);
    return 0;
}

bool VisionGProducer::Start() {
    if (!initialized_.load()) {
        LOG_ERROR("Not initialized");
        return false;
    }

    if (running_.load()) {
        LOG_WARN("Already running");
        return true;
    }

    running_.store(true);
    frame_thread_ = std::thread(&VisionGProducer::FrameLoop, this);
    LOG_INFO("VisionG producer ({}) started", type_name_);
    return true;
}

void VisionGProducer::Stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);
    if (frame_thread_.joinable()) {
        frame_thread_.join();
    }
    LOG_INFO("VisionG producer ({}) stopped", type_name_);
}

void VisionGProducer::RegisterStreamConsumer(const std::string& name, StreamCallback callback,
                                              StreamConsumerType type, int queue_size) {
    (void)queue_size;
    impl_->dispatcher.RegisterConsumer(name, std::move(callback), type);
}

void VisionGProducer::ClearStreamConsumers() {
    impl_->dispatcher.ClearConsumers();
}

int VisionGProducer::SetResolution(Resolution preset) {
    if (running_.load()) {
        LOG_WARN("Cannot change resolution while running");
        return -1;
    }
    config_.resolution = preset;
    return 0;
}

int VisionGProducer::SetFrameRate(int fps) {
    if (running_.load()) {
        LOG_WARN("Cannot change framerate while running");
        return -1;
    }
    fps = std::max(1, std::min(fps, 30));
    config_.framerate = fps;
    return 0;
}

// ============================================================================
// VENC 初始化
// ============================================================================

int VisionGProducer::InitVenc() {
    auto res = config_.GetResolutionConfig();

    // 初始化 VENC 通道
    if (InitVencChannel(res.width, res.height) != 0) {
        return -1;
    }
    venc_enabled_ = true;

    // 创建 RGB MB Pool（1 block 预分配，与 yolo_producer 一致）
    size_t block_size = static_cast<size_t>(res.width) * res.height * 3;
    MB_POOL_CONFIG_S cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.u64MBSize = block_size;
    cfg.u32MBCnt = 1;
    cfg.enAllocType = MB_ALLOC_TYPE_DMA;

    impl_->rgb_pool = RK_MPI_MB_CreatePool(&cfg);
    if (impl_->rgb_pool == MB_INVALID_POOLID) {
        LOG_ERROR("Failed to create MB pool");
        return -1;
    }

    impl_->rgb_blk = RK_MPI_MB_GetMB(impl_->rgb_pool, block_size, RK_TRUE);
    if (impl_->rgb_blk == MB_INVALID_HANDLE) {
        LOG_ERROR("Failed to get RGB MB block");
        return -1;
    }

    impl_->rgb_block_size = block_size;
    LOG_DEBUG("VENC initialized with MB pool: {} bytes", block_size);
    return 0;
}

void VisionGProducer::DeinitVenc() {
    if (venc_enabled_) {
        RK_MPI_VENC_StopRecvFrame(kVencChn);
        RK_MPI_VENC_DestroyChn(kVencChn);
        venc_enabled_ = false;
    }
}

// ============================================================================
// 帧处理循环
// ============================================================================

void VisionGProducer::FrameLoop() {
    LOG_INFO("VisionG frame loop started ({})", type_name_);

    auto res = config_.GetResolutionConfig();
    int width = res.width;
    int height = res.height;

    // 预构建 h264_frame 结构体（与 yolo_producer 一致）
    VIDEO_FRAME_INFO_S h264_frame;
    memset(&h264_frame, 0, sizeof(h264_frame));
    h264_frame.stVFrame.u32Width = width;
    h264_frame.stVFrame.u32Height = height;
    h264_frame.stVFrame.u32VirWidth = width;
    h264_frame.stVFrame.u32VirHeight = height;
    h264_frame.stVFrame.enPixelFormat = RK_FMT_RGB888;
    h264_frame.stVFrame.u32FrameFlag = 160;
    h264_frame.stVFrame.pMbBlk = impl_->rgb_blk;

    // VENC buffer 指针
    unsigned char* venc_data = (unsigned char*)RK_MPI_MB_Handle2VirAddr(impl_->rgb_blk);

    RK_U32 H264_TimeRef = 0;
    RK_S32 s32Ret;

    LOG_INFO("FrameLoop entering main loop: Camera → NPU → OSD → VENC");

    while (running_.load()) {
        h264_frame.stVFrame.u32TimeRef = H264_TimeRef++;
        h264_frame.stVFrame.u64PTS = GetNowUs();

        // 1. 使用 VisionG Camera 取帧
        ImageBuffer frame = camera_->snapshot();
        if (!frame.is_valid()) {
            LOG_WARN("Camera snapshot failed, retrying...");
            continue;
        }
        frame_count_++;

        // 2. 使用 VisionG NPU 推理
        auto detections = npu_->inference(frame);
        inference_count_++;

        // 3. 获取 BGR 版本用于绘制 OSD
        //    VisionG ImageBuffer 支持 get_bgr_version() 缓存转换
        const ImageBuffer& bgr_frame = frame.get_bgr_version();

        // 4. 使用 VisionG ImageBuffer 在 BGR 帧上画框
        //    注意：draw 方法返回引用（就地修改），所以需要 mutable 拷贝
        ImageBuffer draw_frame = bgr_frame.copy();
        for (const auto& det : detections) {
            auto [x, y, w, h] = det.box;

            LOG_DEBUG("{} @ ({} {} {} {}) {:.3f}", det.label, x, y, w, h, det.score);

            draw_frame.draw_rectangle(x, y, w, h, {0, 255, 0}, 3, false);

            char text[64];
            snprintf(text, sizeof(text), "%s %.1f%%", det.label.c_str(), det.score * 100);
            draw_frame.draw_string(x, y - 8, text, {0, 255, 0}, 1.0, 2);
        }

        // 5. 拷贝 RGB 数据到 VENC buffer
        const void* rgb_data = draw_frame.get_data();
        size_t rgb_size = static_cast<size_t>(width) * height * 3;
        if (rgb_data && draw_frame.get_size() >= rgb_size) {
            memcpy(venc_data, rgb_data, rgb_size);
        } else {
            LOG_WARN("Draw frame data invalid, skipping VENC");
            continue;
        }

        // 6. VENC 编码
        s32Ret = RK_MPI_VENC_SendFrame(kVencChn, &h264_frame, -1);
        if (s32Ret != RK_SUCCESS) {
            LOG_WARN("VENC SendFrame fail: {:#x}", s32Ret);
            continue;
        }

        // 7. 获取编码流并分发
        RK_S32 last_error = 0;
        auto stream = acquire_encoded_stream(kVencChn, 1000, &last_error);
        if (stream) {
            impl_->dispatcher.DispatchFrame(stream);
        } else {
            LOG_WARN("VENC GetStream fail: {:#x}", last_error);
        }
    }

    LOG_INFO("VisionG frame loop exited ({}), total frames: {}, inferences: {}",
             type_name_, frame_count_.load(), inference_count_.load());
}

// ============================================================================
// 工厂函数实现
// ============================================================================

std::unique_ptr<IMediaProducer> CreateVisionGYoloProducer(const ProducerConfig& config) {
    return std::make_unique<VisionGProducer>(
        config,
        ModelType::YOLOV5,
        "../model/yolov5.rknn",
        "../model/coco_80_labels_list.txt"
    );
}

std::unique_ptr<IMediaProducer> CreateVisionGRetinaFaceProducer(const ProducerConfig& config) {
    return std::make_unique<VisionGProducer>(
        config,
        ModelType::RETINAFACE,
        "../model/retinaface.rknn",
        ""
    );
}

} // namespace media
