/**
 * @file simple_ipc_producer.cpp
 * @brief 纯监控模式生产者实现
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#define LOG_TAG "SimpleIPC"

#include "simple_ipc_producer.h"
#include "mpi_config.h"
#include "common/logger.h"
#include "common/media_buffer.h"
#include "common/asio_context.h"

#include "sample_comm.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_vpss.h"
#include "rk_mpi_venc.h"

#include <thread>
#include <cstring>

namespace media {

// ============================================================================
// 流分发器（嵌入实现）
// ============================================================================

/**
 * @brief 简化的流分发器
 * 
 * 直接在 Fetch 线程中分发，不使用额外队列
 */
class SimpleStreamDispatcher {
public:
    struct ConsumerInfo {
        std::string name;
        StreamCallback callback;
        StreamConsumerType type;
    };

    void RegisterConsumer(const std::string& name, StreamCallback callback, 
                          StreamConsumerType type) {
        consumers_.push_back({name, std::move(callback), type});
        LOG_INFO("Registered stream consumer: {} (type={})", name, 
                 type == StreamConsumerType::AsyncIO ? "AsyncIO" : "Direct");
    }

    void ClearConsumers() {
        consumers_.clear();
    }

    void Start(int venc_chn) {
        if (running_) return;
        venc_chn_ = venc_chn;
        running_ = true;
        fetch_thread_ = std::thread(&SimpleStreamDispatcher::FetchLoop, this);
        LOG_INFO("Stream dispatcher started for VENC channel {}", venc_chn);
    }

    void Stop() {
        if (!running_) return;
        running_ = false;
        if (fetch_thread_.joinable()) {
            fetch_thread_.join();
        }
        LOG_INFO("Stream dispatcher stopped");
    }

    bool IsRunning() const { return running_; }

private:
    void FetchLoop() {
        uint64_t frame_count = 0;
        uint64_t consecutive_errors = 0;

        while (running_) {
            RK_S32 last_error = 0;
            auto stream = acquire_encoded_stream(venc_chn_, 1000, &last_error);

            if (!stream) {
                consecutive_errors++;
                if (consecutive_errors > 3 && consecutive_errors % 10 == 0) {
                    LOG_WARN("VENC consecutive errors: {}, last: {:#x}", 
                             consecutive_errors, last_error);
                }
                continue;
            }

            frame_count++;
            consecutive_errors = 0;

            if (frame_count <= 5 || frame_count % 300 == 0) {
                LOG_DEBUG("Frame #{}, size={} bytes", frame_count, 
                         stream->pstPack ? stream->pstPack->u32Len : 0);
            }

            // 分发给所有消费者
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

        LOG_DEBUG("Fetch loop exited, total frames: {}", frame_count);
    }

    std::vector<ConsumerInfo> consumers_;
    std::atomic<bool> running_{false};
    std::thread fetch_thread_;
    int venc_chn_ = 0;
};

// ============================================================================
// SimpleIPCProducer 内部实现
// ============================================================================

struct SimpleIPCProducer::Impl {
    // MPI 状态
    bool isp_initialized = false;
    bool mpi_initialized = false;
    bool vi_enabled = false;
    bool vpss_enabled = false;
    bool venc_enabled = false;
    
    // 绑定句柄
    MPP_CHN_S vi_chn;
    MPP_CHN_S vpss_grp;
    MPP_CHN_S vpss_chn0;
    MPP_CHN_S venc_chn;
    
    // 流分发器
    SimpleStreamDispatcher dispatcher;
};

// ============================================================================
// SimpleIPCProducer 实现
// ============================================================================

SimpleIPCProducer::SimpleIPCProducer(const ProducerConfig& config)
    : config_(config)
    , impl_(std::make_unique<Impl>()) {
    LOG_DEBUG("SimpleIPCProducer created");
}

SimpleIPCProducer::~SimpleIPCProducer() {
    Deinit();
    LOG_DEBUG("SimpleIPCProducer destroyed");
}

int SimpleIPCProducer::Init() {
    if (initialized_.load()) {
        LOG_WARN("Already initialized");
        return 0;
    }

    auto res = config_.GetResolutionConfig();
    LOG_INFO("Initializing SimpleIPC producer: {}x{} @ {}fps", 
             res.width, res.height, res.framerate);

    if (InitMpi() != 0) {
        LOG_ERROR("Failed to initialize MPI");
        return -1;
    }

    if (SetupBindings() != 0) {
        LOG_ERROR("Failed to setup bindings");
        DeinitMpi();
        return -1;
    }

    initialized_.store(true);
    LOG_INFO("SimpleIPC producer initialized successfully");
    LOG_INFO("Pipeline: VI -> VPSS -> VENC (hardware bound, zero-copy)");
    return 0;
}

int SimpleIPCProducer::Deinit() {
    if (!initialized_.load()) {
        return 0;
    }

    Stop();
    TeardownBindings();
    DeinitMpi();

    initialized_.store(false);
    LOG_INFO("SimpleIPC producer deinitialized");
    return 0;
}

bool SimpleIPCProducer::Start() {
    if (!initialized_.load()) {
        LOG_ERROR("Not initialized");
        return false;
    }

    if (running_.load()) {
        LOG_WARN("Already running");
        return true;
    }

    impl_->dispatcher.Start(kVencChn);
    running_.store(true);
    LOG_INFO("SimpleIPC producer started");
    return true;
}

void SimpleIPCProducer::Stop() {
    if (!running_.load()) {
        return;
    }

    impl_->dispatcher.Stop();
    running_.store(false);
    LOG_INFO("SimpleIPC producer stopped");
}

void SimpleIPCProducer::RegisterStreamConsumer(
    const std::string& name,
    StreamCallback callback,
    StreamConsumerType type,
    int queue_size) {
    (void)queue_size;  // 不使用队列
    impl_->dispatcher.RegisterConsumer(name, std::move(callback), type);
}

void SimpleIPCProducer::ClearStreamConsumers() {
    impl_->dispatcher.ClearConsumers();
}

int SimpleIPCProducer::SetResolution(Resolution preset) {
    if (running_.load()) {
        LOG_WARN("Cannot change resolution while running");
        return -1;
    }
    config_.resolution = preset;
    LOG_INFO("Resolution set to {}", ResolutionConfig::FromPreset(preset).width);
    return 0;
}

int SimpleIPCProducer::SetFrameRate(int fps) {
    if (running_.load()) {
        LOG_WARN("Cannot change framerate while running");
        return -1;
    }
    fps = std::max(1, std::min(fps, 30));
    config_.framerate = fps;
    LOG_INFO("Frame rate set to {}", fps);
    return 0;
}

// ============================================================================
// MPI 初始化
// ============================================================================

int SimpleIPCProducer::InitMpi() {
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

    // 4. VPSS 初始化（单通道，给 VENC）
    ret = vpss_init(kVpssGrp, res.width, res.height, res.width, res.height, 0, 0);
    if (ret != 0) {
        LOG_ERROR("VPSS init failed");
        return -1;
    }
    impl_->vpss_enabled = true;
    LOG_DEBUG("VPSS initialized");

    // 5. VENC 初始化
    ret = venc_init(kVencChn, res.width, res.height, RK_VIDEO_ID_AVC);
    if (ret != 0) {
        LOG_ERROR("VENC init failed");
        return -1;
    }
    impl_->venc_enabled = true;
    LOG_DEBUG("VENC initialized");

    return 0;
}

int SimpleIPCProducer::DeinitMpi() {
    // VENC
    if (impl_->venc_enabled) {
        RK_MPI_VENC_StopRecvFrame(kVencChn);
        
        // 排空 VENC
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
        LOG_DEBUG("VENC deinitialized");
    }

    // VPSS
    if (impl_->vpss_enabled) {
        vpss_deinit(kVpssGrp, false);
        impl_->vpss_enabled = false;
        LOG_DEBUG("VPSS deinitialized");
    }

    // VI
    if (impl_->vi_enabled) {
        RK_MPI_VI_DisableChn(kViDev, kViChn);
        RK_MPI_VI_DisableDev(kViDev);
        impl_->vi_enabled = false;
        LOG_DEBUG("VI deinitialized");
    }

    // MPI 系统
    if (impl_->mpi_initialized) {
        RK_MPI_SYS_Exit();
        impl_->mpi_initialized = false;
        LOG_DEBUG("MPI system deinitialized");
    }

    // ISP
    if (impl_->isp_initialized) {
        SAMPLE_COMM_ISP_Stop(kViDev);
        impl_->isp_initialized = false;
        LOG_DEBUG("ISP stopped");
    }

    return 0;
}

int SimpleIPCProducer::SetupBindings() {
    RK_S32 ret;

    // VI -> VPSS
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

    // VPSS Chn0 -> VENC
    impl_->vpss_chn0.enModId = RK_ID_VPSS;
    impl_->vpss_chn0.s32DevId = kVpssGrp;
    impl_->vpss_chn0.s32ChnId = kVpssChn0;

    impl_->venc_chn.enModId = RK_ID_VENC;
    impl_->venc_chn.s32DevId = 0;
    impl_->venc_chn.s32ChnId = kVencChn;

    ret = RK_MPI_SYS_Bind(&impl_->vpss_chn0, &impl_->venc_chn);
    if (ret != RK_SUCCESS) {
        LOG_ERROR("Failed to bind VPSS -> VENC: {:#x}", ret);
        return -1;
    }
    LOG_DEBUG("VPSS -> VENC bound");

    return 0;
}

void SimpleIPCProducer::TeardownBindings() {
    RK_MPI_SYS_UnBind(&impl_->vpss_chn0, &impl_->venc_chn);
    LOG_DEBUG("VPSS -> VENC unbound");

    impl_->vpss_grp.s32ChnId = 0;
    RK_MPI_SYS_UnBind(&impl_->vi_chn, &impl_->vpss_grp);
    LOG_DEBUG("VI -> VPSS unbound");
}

// ============================================================================
// 工厂函数实现
// ============================================================================

std::unique_ptr<IMediaProducer> CreateSimpleIPCProducer(const ProducerConfig& config) {
    return std::make_unique<SimpleIPCProducer>(config);
}

}  // namespace media
