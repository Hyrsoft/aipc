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

static MPP_CHN_S stSrcChn, stvencChn;
static VideoConfig g_videoConfig;
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

    // vi init
    LOG_DEBUG("Initializing VI device...");
    vi_dev_init();

    // 创建两个vi通道，通道0连接venc进行编码，通道1用于NPU处理
    LOG_DEBUG("Initializing VI channels 0 and 1...");
    vi_chn_init(0, width, height);
    vi_chn_init(1, width, height);
    LOG_INFO("VI initialized with 2 channels");
    
    // venc init
    RK_CODEC_ID_E enCodecType = RK_VIDEO_ID_AVC;
    LOG_DEBUG("Initializing VENC with H.264 codec...");
    venc_init(0, width, height, enCodecType);
    LOG_INFO("VENC initialized successfully");

    // bind vi to venc    
    stSrcChn.enModId = RK_ID_VI;
    stSrcChn.s32DevId = 0;
    stSrcChn.s32ChnId = 0;
        
    stvencChn.enModId = RK_ID_VENC;
    stvencChn.s32DevId = 0;
    stvencChn.s32ChnId = 0;
    LOG_DEBUG("Binding VI channel 0 to VENC channel 0...");
    s32Ret = RK_MPI_SYS_Bind(&stSrcChn, &stvencChn);
    if (s32Ret != RK_SUCCESS) {
        LOG_ERROR("Failed to bind VI to VENC, error code: {:#x}", s32Ret);
        return -1;
    }
    LOG_INFO("VI to VENC binding successful");

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
    return 0;
}

int rkvideo_deinit() {
    LOG_INFO("Deinitializing rkvideo module...");

    // 停止流分发
    if (g_dispatcher) {
        g_dispatcher->Stop();
        g_dispatcher.reset();
    }

    LOG_DEBUG("Unbinding VI from VENC...");
    RK_MPI_SYS_UnBind(&stSrcChn, &stvencChn);

    LOG_DEBUG("Disabling VI channels...");
    RK_MPI_VI_DisableChn(0, 0);
    RK_MPI_VI_DisableChn(0, 1);
    
    LOG_DEBUG("Stopping and destroying VENC...");
    RK_MPI_VENC_StopRecvFrame(0);
    RK_MPI_VENC_DestroyChn(0);
    
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
    // 从 VI 通道 1 获取原始帧（用于 AI 推理）
    return acquire_video_frame(0, 1, timeoutMs);
}

const VideoConfig& rkvideo_get_config() {
    return g_videoConfig;
}
