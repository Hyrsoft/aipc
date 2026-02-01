// 定义模块名，必须在 #include "logger.h" 之前
#define LOG_TAG "rkvideo"

#include "rkvideo.h"
#include "luckfox_mpi.h"
#include "stream_dispatcher.h"
#include "common/logger.h"

#include <memory>
#include <vector>
#include <mutex>

// ============================================================================
// 模块内部状态
// ============================================================================

// 绑定关系：VI -> VPSS -> VENC
static MPP_CHN_S stViChn;     // VI 通道
static MPP_CHN_S stVpssChn0;  // VPSS Chn0（绑定到 VENC）
static MPP_CHN_S stVencChn;   // VENC 通道

static VideoConfig g_videoConfig;

// 是否启用 AI 推理通道（VPSS Chn1）
static bool g_enableAiChannel = true;

// 分发器的实例
static std::unique_ptr<StreamDispatcher> g_dispatcher;

// 消费者注册信息（在启动前注册）
struct ConsumerRegistration {
    std::string name;
    VideoStreamCallback callback;
    void* userData;
    int queueSize;
};
static std::vector<ConsumerRegistration> g_pendingConsumers;
static std::mutex g_consumerMutex;

// ============================================================================
// 初始化与反初始化
// ============================================================================

int rkvideo_init() {
    LOG_INFO("Initializing rkvideo module...");

    RK_S32 s32Ret = 0; 

    int width    = g_videoConfig.width;
    int height   = g_videoConfig.height;

    LOG_DEBUG("Display resolution: {}x{}", width, height);

    // rkaiq init 
    RK_BOOL multi_sensor = RK_FALSE;    
    const char *iq_dir = "/etc/iqfiles"; // IQ文件路径
    rk_aiq_working_mode_t hdr_mode = RK_AIQ_WORKING_MODE_NORMAL;
    LOG_DEBUG("Initializing ISP with IQ path: {}", iq_dir);
    SAMPLE_COMM_ISP_Init(0, hdr_mode, multi_sensor, iq_dir);
    SAMPLE_COMM_ISP_Run(0);
    LOG_INFO("ISP initialized successfully");

    // rkmpi init
    LOG_DEBUG("Initializing RK MPI system...");
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        LOG_ERROR("RK MPI system init failed!");
        return -1;
    }
    LOG_INFO("RK MPI system initialized successfully");

    // ==================== VI 初始化（单通道）====================
    LOG_DEBUG("Initializing VI device...");
    vi_dev_init();

    // 只创建一个 VI 通道，绑定到 VPSS
    LOG_DEBUG("Initializing VI channel 0 ({}x{})...", width, height);
    vi_chn_init(0, width, height);
    LOG_INFO("VI initialized with single channel");
    
    // ==================== VPSS 初始化（分流）====================
    // VPSS Chn0: 全分辨率 -> VENC（编码流）
    // VPSS Chn1: 全分辨率 -> User GetFrame（AI 推理，可选）
    LOG_DEBUG("Initializing VPSS Group 0...");
    if (g_enableAiChannel) {
        // 启用 AI 通道：Chn0 给 VENC，Chn1 给 AI
        s32Ret = vpss_init(0, width, height, width, height, width, height);
    } else {
        // 不启用 AI 通道：只有 Chn0 给 VENC
        s32Ret = vpss_init(0, width, height, width, height, 0, 0);
    }
    if (s32Ret != 0) {
        LOG_ERROR("VPSS init failed!");
        return -1;
    }
    LOG_INFO("VPSS initialized with {} channels", g_enableAiChannel ? 2 : 1);

    // ==================== VENC 初始化 ====================
    RK_CODEC_ID_E enCodecType = RK_VIDEO_ID_AVC;
    LOG_DEBUG("Initializing VENC with H.264 codec...");
    venc_init(0, width, height, enCodecType);
    LOG_INFO("VENC initialized successfully");

    // ==================== 绑定：VI -> VPSS -> VENC ====================
    // 1. 绑定 VI Chn0 -> VPSS Group0 (输入)
    stViChn.enModId = RK_ID_VI;
    stViChn.s32DevId = 0;
    stViChn.s32ChnId = 0;
    
    stVpssChn0.enModId = RK_ID_VPSS;
    stVpssChn0.s32DevId = 0;  // VPSS Group ID
    stVpssChn0.s32ChnId = 0;  // 这里用于绑定时指 Group，不是 Channel
    
    LOG_DEBUG("Binding VI Chn0 -> VPSS Group0...");
    s32Ret = RK_MPI_SYS_Bind(&stViChn, &stVpssChn0);
    if (s32Ret != RK_SUCCESS) {
        LOG_ERROR("Failed to bind VI to VPSS, error code: {:#x}", s32Ret);
        return -1;
    }
    LOG_INFO("VI -> VPSS binding successful");
    
    // 2. 绑定 VPSS Chn0 -> VENC Chn0
    stVpssChn0.enModId = RK_ID_VPSS;
    stVpssChn0.s32DevId = 0;  // VPSS Group ID
    stVpssChn0.s32ChnId = 0;  // VPSS Channel ID
    
    stVencChn.enModId = RK_ID_VENC;
    stVencChn.s32DevId = 0;
    stVencChn.s32ChnId = 0;
    
    LOG_DEBUG("Binding VPSS Chn0 -> VENC Chn0...");
    s32Ret = RK_MPI_SYS_Bind(&stVpssChn0, &stVencChn);
    if (s32Ret != RK_SUCCESS) {
        LOG_ERROR("Failed to bind VPSS to VENC, error code: {:#x}", s32Ret);
        return -1;
    }
    LOG_INFO("VPSS -> VENC binding successful");

    // 创建流分发器
    g_dispatcher = std::make_unique<StreamDispatcher>(0);
    
    // 注册已登记的消费者
    {
        std::lock_guard<std::mutex> lock(g_consumerMutex);
        for (const auto& reg : g_pendingConsumers) {
            g_dispatcher->RegisterConsumer(reg.name, 
                [reg](EncodedStreamPtr stream) {
                    if (reg.callback) {
                        reg.callback(stream, reg.userData);
                    }
                }, 
                reg.queueSize);
        }
        g_pendingConsumers.clear();
    }
            
    LOG_INFO("rkvideo module initialized successfully");
    LOG_INFO("Pipeline: VI -> VPSS (Chn0->VENC{}) -> H.264 Stream",
             g_enableAiChannel ? ", Chn1->AI" : "");
    return 0;
}

int rkvideo_deinit() {
    LOG_INFO("Deinitializing rkvideo module...");

    // 停止流分发
    if (g_dispatcher) {
        g_dispatcher->Stop();
        g_dispatcher.reset();
    }

    // 解除绑定（顺序与绑定相反）
    LOG_DEBUG("Unbinding VPSS from VENC...");
    RK_MPI_SYS_UnBind(&stVpssChn0, &stVencChn);
    
    LOG_DEBUG("Unbinding VI from VPSS...");
    stVpssChn0.s32ChnId = 0;  // 恢复为绑定时的值
    RK_MPI_SYS_UnBind(&stViChn, &stVpssChn0);

    // 销毁 VENC
    LOG_DEBUG("Stopping and destroying VENC...");
    RK_MPI_VENC_StopRecvFrame(0);
    RK_MPI_VENC_DestroyChn(0);
    
    // 销毁 VPSS
    LOG_DEBUG("Destroying VPSS...");
    vpss_deinit(0, g_enableAiChannel);

    // 销毁 VI
    LOG_DEBUG("Disabling VI channel 0...");
    RK_MPI_VI_DisableChn(0, 0);
    
    LOG_DEBUG("Disabling VI device...");
    RK_MPI_VI_DisableDev(0);

    LOG_DEBUG("Exiting RK MPI system...");
    RK_MPI_SYS_Exit();

    // Stop RKAIQ
    LOG_DEBUG("Stopping ISP...");
    SAMPLE_COMM_ISP_Stop(0);

    LOG_INFO("rkvideo module deinitialized successfully");
    return 0;
}

// ============================================================================
// 流分发接口
// ============================================================================

void rkvideo_register_stream_consumer(const char* name, VideoStreamCallback callback, 
                                       void* userData, int queueSize) {
    std::lock_guard<std::mutex> lock(g_consumerMutex);
    
    if (g_dispatcher) {
        // 如果分发器已创建，直接注册
        g_dispatcher->RegisterConsumer(name, 
            [callback, userData](EncodedStreamPtr stream) {
                if (callback) {
                    callback(stream, userData);
                }
            }, 
            queueSize);
    } else {
        // 否则先存储，等初始化时再注册
        g_pendingConsumers.push_back({name, callback, userData, queueSize});
    }
    
    LOG_INFO("Stream consumer registered: {}", name);
}

void rkvideo_start_streaming() {
    if (g_dispatcher) {
        g_dispatcher->Start();
        LOG_INFO("Streaming started");
    }
}

void rkvideo_stop_streaming() {
    if (g_dispatcher) {
        g_dispatcher->Stop();
        LOG_INFO("Streaming stopped");
    }
}

// ============================================================================
// 原始帧接口
// ============================================================================

VideoFramePtr rkvideo_get_vi_frame(int timeoutMs) {
    // 从 VPSS Chn1 获取原始帧（用于 AI 推理）
    // 新架构：VI -> VPSS Group0
    //         ├── VPSS Chn0 -> VENC（编码流）
    //         └── VPSS Chn1 -> User GetFrame（AI 推理）
    if (!g_enableAiChannel) {
        LOG_WARN("AI channel (VPSS Chn1) is not enabled!");
        return nullptr;
    }
    return acquire_vpss_frame(0, VPSS_CHN1, timeoutMs);
}

const VideoConfig& rkvideo_get_config() {
    return g_videoConfig;
}
