/**
 * @file yolo_producer.cpp
 * @brief YOLOv5 AI 推理模式生产者实现
 *
 * 对齐 luckfox_pico_rtsp_yolov5 例程：
 * - VI 直接取帧（无 VPSS）
 * - NV12 → BGR (OpenCV) → letterbox → RKNN 推理 → 画框 → VENC 编码
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#define LOG_TAG "YoloProd"

#include "yolo_producer.h"
#include "../common/image_utils.h"
#include "common/asio_context.h"
#include "common/logger.h"
#include "common/media_buffer.h"
#include "mpi_config.h"
#include "yolov5_model.h"

#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"

#include "opencv2/core/core.hpp"
#include "opencv2/imgproc/imgproc.hpp"

#include <cstring>
#include <ctime>

namespace media {

    // ============================================================================
    // 流分发器（与 SimpleIPCProducer 共用逻辑）
    // ============================================================================

    class SerialStreamDispatcher {
    public:
        struct ConsumerInfo {
            std::string name;
            StreamCallback callback;
            StreamConsumerType type;
        };

        void RegisterConsumer(const std::string &name, StreamCallback callback, StreamConsumerType type) {
            consumers_.push_back({name, std::move(callback), type});
            LOG_INFO("Registered stream consumer: {}", name);
        }

        void ClearConsumers() { consumers_.clear(); }

        void DispatchFrame(EncodedStreamPtr stream) {
            for (auto &c: consumers_) {
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
    // MB Pool RAII 封装（与例程一致：预分配 1 个 block）
    // ============================================================================

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
            if (pool_ == MB_INVALID_POOLID)
                return MB_INVALID_HANDLE;
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
    // YoloProducer 内部实现
    // ============================================================================

    struct YoloProducer::Impl {
        // MPI 状态
        bool isp_initialized = false;
        bool mpi_initialized = false;
        bool vi_enabled = false;
        bool venc_enabled = false;

        // RGB 缓冲池（与例程一致：1 个 block，预先分配）
        MbPool rgb_pool;
        MB_BLK rgb_blk = MB_INVALID_HANDLE;

        // AI 引擎
        std::unique_ptr<rknn::YoloV5Model> ai_model;
        std::unique_ptr<rknn::ImageProcessor> image_processor;

        // 流分发器
        SerialStreamDispatcher dispatcher;
    };

    // ============================================================================
    // YoloProducer 实现
    // ============================================================================

    YoloProducer::YoloProducer(const ProducerConfig &config) : config_(config), impl_(std::make_unique<Impl>()) {
        LOG_DEBUG("YoloProducer created");
    }

    YoloProducer::~YoloProducer() {
        Deinit();
        LOG_DEBUG("YoloProducer destroyed");
    }

    int YoloProducer::Init() {
        if (initialized_.load()) {
            LOG_WARN("Already initialized");
            return 0;
        }

        auto res = config_.GetResolutionConfig();
        LOG_INFO("Initializing Yolo producer: {}x{} @ {}fps, AI: {}x{}", res.width, res.height, res.framerate,
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
        LOG_INFO("Yolo producer initialized successfully");
        LOG_INFO("Pipeline: VI --(manual GetChnFrame)--> NPU --> VENC (no VPSS)");
        return 0;
    }

    int YoloProducer::Deinit() {
        if (!initialized_.load()) {
            return 0;
        }

        Stop();
        DeinitAiEngine();

        // 释放预分配的 MB Block
        if (impl_->rgb_blk != MB_INVALID_HANDLE) {
            RK_MPI_MB_ReleaseMB(impl_->rgb_blk);
            impl_->rgb_blk = MB_INVALID_HANDLE;
        }
        impl_->rgb_pool.Destroy();

        DeinitMpi();

        initialized_.store(false);
        LOG_INFO("Yolo producer deinitialized");
        return 0;
    }

    bool YoloProducer::Start() {
        if (!initialized_.load()) {
            LOG_ERROR("Not initialized");
            return false;
        }

        if (running_.load()) {
            LOG_WARN("Already running");
            return true;
        }

        running_.store(true);
        frame_thread_ = std::thread(&YoloProducer::FrameLoop, this);
        LOG_INFO("Yolo producer started");
        return true;
    }

    void YoloProducer::Stop() {
        if (!running_.load()) {
            return;
        }

        running_.store(false);
        if (frame_thread_.joinable()) {
            frame_thread_.join();
        }
        LOG_INFO("Yolo producer stopped");
    }

    void YoloProducer::RegisterStreamConsumer(const std::string &name, StreamCallback callback, StreamConsumerType type,
                                              int queue_size) {
        (void) queue_size;
        impl_->dispatcher.RegisterConsumer(name, std::move(callback), type);
    }

    void YoloProducer::ClearStreamConsumers() { impl_->dispatcher.ClearConsumers(); }

    int YoloProducer::SetResolution(Resolution preset) {
        if (running_.load()) {
            LOG_WARN("Cannot change resolution while running");
            return -1;
        }
        config_.resolution = preset;
        return 0;
    }

    int YoloProducer::SetFrameRate(int fps) {
        if (running_.load()) {
            LOG_WARN("Cannot change framerate while running");
            return -1;
        }
        fps = std::max(1, std::min(fps, 30));
        config_.framerate = fps;
        return 0;
    }

    // ============================================================================
    // MPI 初始化（对齐例程 main.cc 的初始化顺序：ISP → SYS → VI → VENC）
    // ============================================================================

    int YoloProducer::InitMpi() {
        auto res = config_.GetResolutionConfig();
        RK_S32 ret;

        // 1. ISP 初始化（与例程一致）
        RK_BOOL multi_sensor = RK_FALSE;
        const char *iq_dir = "/etc/iqfiles";
        rk_aiq_working_mode_t hdr_mode = RK_AIQ_WORKING_MODE_NORMAL;
        SAMPLE_COMM_ISP_Init(kViDev, hdr_mode, multi_sensor, iq_dir);
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

        // 3. VI 初始化（与例程完全一致，u32Depth=2，无 VPSS）
        ret = vi_dev_init();
        if (ret != 0) {
            LOG_ERROR("vi_dev_init failed: {}", ret);
            return -1;
        }
        LOG_DEBUG("vi_dev_init OK");

        ret = vi_chn_init(kViChn, res.width, res.height);
        if (ret != 0) {
            LOG_ERROR("vi_chn_init failed: {:#x}", ret);
            return -1;
        }
        impl_->vi_enabled = true;
        LOG_DEBUG("VI initialized: {}x{} (u32Depth=2, direct GetChnFrame)", res.width, res.height);

        // 4. VENC 初始化（与例程一致，RGB888 输入）
        RK_CODEC_ID_E enCodecType = RK_VIDEO_ID_AVC;
        venc_init(kVencChn, res.width, res.height, enCodecType);
        impl_->venc_enabled = true;
        LOG_DEBUG("VENC initialized (RGB input mode)");

        return 0;
    }

    int YoloProducer::DeinitMpi() {
        // VENC
        if (impl_->venc_enabled) {
            RK_MPI_VENC_StopRecvFrame(kVencChn);
            RK_MPI_VENC_DestroyChn(kVencChn);
            impl_->venc_enabled = false;
        }

        // VI
        if (impl_->vi_enabled) {
            RK_MPI_VI_DisableChn(kViDev, kViChn);
            RK_MPI_VI_DisableDev(kViDev);
            impl_->vi_enabled = false;
        }

        // ISP
        if (impl_->isp_initialized) {
            SAMPLE_COMM_ISP_Stop(kViDev);
            impl_->isp_initialized = false;
        }

        // MPI 系统
        if (impl_->mpi_initialized) {
            RK_MPI_SYS_Exit();
            impl_->mpi_initialized = false;
        }

        return 0;
    }

    int YoloProducer::InitAiEngine() {
        // 初始化图像处理器
        impl_->image_processor = std::make_unique<rknn::ImageProcessor>();
        if (!impl_->image_processor->Init(config_.ai_width, config_.ai_height)) {
            LOG_ERROR("Failed to init image processor");
            return -1;
        }

        // 初始化 AI 模型
        impl_->ai_model = std::make_unique<rknn::YoloV5Model>();

        rknn::ModelConfig model_cfg;
        model_cfg.model_path = kDefaultModelPath;
        model_cfg.labels_path = kDefaultLabelsPath;
        model_cfg.conf_threshold = 0.25f;
        model_cfg.nms_threshold = 0.45f;

        if (impl_->ai_model->Init(model_cfg) != 0) {
            LOG_ERROR("Failed to init YOLOv5 model");
            return -1;
        }

        LOG_INFO("AI engine initialized: YOLOv5 {}x{}", config_.ai_width, config_.ai_height);
        return 0;
    }

    void YoloProducer::DeinitAiEngine() {
        if (impl_->ai_model) {
            impl_->ai_model->Deinit();
            impl_->ai_model.reset();
        }
        if (impl_->image_processor) {
            impl_->image_processor->Deinit();
            impl_->image_processor.reset();
        }
    }

    bool YoloProducer::InitRgbPool() {
        auto res = config_.GetResolutionConfig();
        size_t block_size = static_cast<size_t>(res.width) * res.height * 3;

        // 与例程一致：1 个 block，预先分配
        if (!impl_->rgb_pool.Create(block_size, 1)) {
            return false;
        }

        // 预先获取 MB Block（与例程的 RK_MPI_MB_GetMB 一致）
        impl_->rgb_blk = impl_->rgb_pool.GetBlock(true);
        if (impl_->rgb_blk == MB_INVALID_HANDLE) {
            LOG_ERROR("Failed to get RGB MB block");
            return false;
        }

        LOG_DEBUG("RGB pool created: {} bytes x 1 block (pre-allocated)", block_size);
        return true;
    }

    // ============================================================================
    // 辅助函数（与例程 luckfox_mpi.cc 中 TEST_COMM_GetNowUs 一致）
    // ============================================================================

    static RK_U64 TEST_COMM_GetNowUs() {
        struct timespec time = {0, 0};
        clock_gettime(CLOCK_MONOTONIC, &time);
        return (RK_U64) time.tv_sec * 1000000 + (RK_U64) time.tv_nsec / 1000;
    }

    // ============================================================================
    // 帧处理循环（对齐例程 main.cc 的 while(1) 循环）
    // ============================================================================

    void YoloProducer::FrameLoop() {
        LOG_INFO("Yolo frame loop started");

        auto res = config_.GetResolutionConfig();
        int width = res.width;
        int height = res.height;

        // 与例程一致：预构建 h264_frame 结构体
        VIDEO_FRAME_INFO_S h264_frame;
        memset(&h264_frame, 0, sizeof(h264_frame));
        h264_frame.stVFrame.u32Width = width;
        h264_frame.stVFrame.u32Height = height;
        h264_frame.stVFrame.u32VirWidth = width;
        h264_frame.stVFrame.u32VirHeight = height;
        h264_frame.stVFrame.enPixelFormat = RK_FMT_RGB888;
        h264_frame.stVFrame.u32FrameFlag = 160;
        h264_frame.stVFrame.pMbBlk = impl_->rgb_blk;

        // VENC buffer 指针（与例程的 unsigned char *data 一致）
        unsigned char *data = (unsigned char *) RK_MPI_MB_Handle2VirAddr(impl_->rgb_blk);

        RK_U32 H264_TimeRef = 0;

        // 模型尺寸
        int model_width = config_.ai_width;
        int model_height = config_.ai_height;

        RK_S32 s32Ret;
        char text[16];

        LOG_INFO("FrameLoop entering main loop, VI dev={} chn={} (direct GetChnFrame)", kViDev, kViChn);

        while (running_.load()) {
            h264_frame.stVFrame.u32TimeRef = H264_TimeRef++;
            h264_frame.stVFrame.u64PTS = TEST_COMM_GetNowUs();

            // 1. 从 VI 直接获取帧（u32Depth=2，与例程一致）
            VIDEO_FRAME_INFO_S stViFrame;
            s32Ret = RK_MPI_VI_GetChnFrame(kViDev, kViChn, &stViFrame, 1000);
            if (s32Ret != RK_SUCCESS) {
                LOG_WARN("VI GetChnFrame failed: {:#x}, retrying...", s32Ret);
                continue;
            }

            void *vi_data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);
            frame_count_++;

            // 2. NV12 → BGR（与例程一致：bgr 独立分配，不共享 VENC buffer）
            cv::Mat yuv420sp(height + height / 2, width, CV_8UC1, vi_data);
            cv::Mat bgr(height, width, CV_8UC3); // 独立 buffer（与例程一致）
            cv::cvtColor(yuv420sp, bgr, cv::COLOR_YUV420sp2BGR);

            // 3. letterbox（与例程一致）
            float scaleX = (float) model_width / (float) width;
            float scaleY = (float) model_height / (float) height;
            float scale = scaleX < scaleY ? scaleX : scaleY;

            int inputWidth = (int) ((float) width * scale);
            int inputHeight = (int) ((float) height * scale);
            int leftPadding = (model_width - inputWidth) / 2;
            int topPadding = (model_height - inputHeight) / 2;

            cv::Mat inputScale;
            cv::resize(bgr, inputScale, cv::Size(inputWidth, inputHeight), 0, 0, cv::INTER_LINEAR);
            cv::Mat letterboxImage(model_height, model_width, CV_8UC3, cv::Scalar(0, 0, 0));
            cv::Rect roi(leftPadding, topPadding, inputWidth, inputHeight);
            inputScale.copyTo(letterboxImage(roi));

            // 4. 拷贝到 RKNN 输入内存并推理（与例程一致）
            memcpy(impl_->ai_model->GetInputVirtAddr(), letterboxImage.data, model_width * model_height * 3);

            impl_->ai_model->Run();
            inference_count_++;

            // 5. 获取检测结果
            rknn::DetectionResultList od_results;
            impl_->ai_model->GetResults(od_results);

            // 6. 画框（与例程一致：在 bgr 上画，坐标映射 + OpenCV）
            for (size_t i = 0; i < od_results.Count(); i++) {
                const auto &det_result = od_results.results[i];

                int sX = det_result.box.x;
                int sY = det_result.box.y;
                int eX = det_result.box.x + det_result.box.width;
                int eY = det_result.box.y + det_result.box.height;

                // mapCoordinates（与例程一致）
                sX = (int) ((float) (sX - leftPadding) / scale);
                sY = (int) ((float) (sY - topPadding) / scale);
                eX = (int) ((float) (eX - leftPadding) / scale);
                eY = (int) ((float) (eY - topPadding) / scale);

                LOG_DEBUG("{} @ ({} {} {} {}) {:.3f}", det_result.label, sX, sY, eX, eY, det_result.confidence);

                cv::rectangle(bgr, cv::Point(sX, sY), cv::Point(eX, eY), cv::Scalar(0, 255, 0), 3);
                snprintf(text, sizeof(text), "%s %.1f%%", det_result.label.c_str(), det_result.confidence * 100);
                cv::putText(bgr, text, cv::Point(sX, sY - 8), cv::FONT_HERSHEY_SIMPLEX, 1, cv::Scalar(0, 255, 0), 2);
            }

            // 7. 拷贝绘制结果到 VENC buffer（与例程 memcpy(data, bgr.data, ...) 一致）
            memcpy(data, bgr.data, width * height * 3);

            // 8. 释放 VI 帧
            s32Ret = RK_MPI_VI_ReleaseChnFrame(kViDev, kViChn, &stViFrame);
            if (s32Ret != RK_SUCCESS) {
                LOG_WARN("VI ReleaseChnFrame fail: {:#x}", s32Ret);
            }

            // 9. VENC 编码
            s32Ret = RK_MPI_VENC_SendFrame(kVencChn, &h264_frame, -1);
            if (s32Ret != RK_SUCCESS) {
                LOG_WARN("VENC SendFrame fail: {:#x}", s32Ret);
                continue;
            }

            // 10. 获取编码流并分发
            RK_S32 last_error = 0;
            auto stream = acquire_encoded_stream(kVencChn, 1000, &last_error);
            if (stream) {
                impl_->dispatcher.DispatchFrame(stream);
            } else {
                LOG_WARN("VENC GetStream fail: {:#x}", last_error);
            }

            memset(text, 0, sizeof(text));
        }

        LOG_INFO("Yolo frame loop exited, total frames: {}, inferences: {}", frame_count_.load(),
                 inference_count_.load());
    }

    bool YoloProducer::ProcessFrame(void *nv12_data, int width, int height, int stride, void *rgb_output,
                                    uint64_t pts) {
        // 此方法不再使用，帧处理已内联到 FrameLoop 中（与例程一致）
        (void) nv12_data;
        (void) width;
        (void) height;
        (void) stride;
        (void) rgb_output;
        (void) pts;
        return true;
    }


    // ============================================================================
    // 工厂函数实现
    // ============================================================================

    std::unique_ptr<IMediaProducer> CreateYoloProducer(const ProducerConfig &config) {
        return std::make_unique<YoloProducer>(config);
    }

} // namespace media
