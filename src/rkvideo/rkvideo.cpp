// 定义模块名，必须在 #include "logger.h" 之前
#define LOG_TAG "rkvideo"

#include "rkvideo.h"
#include "luckfox_mpi.h"
#include "common/logger.h"

MPP_CHN_S stSrcChn, stSrcChn1, stvpssChn, stvencChn;
VENC_RECV_PIC_PARAM_S stRecvParam;

// 配置参数
#define DISP_WIDTH  720
#define DISP_HEIGHT 480

// rk媒体库的初始化入口，包括vi、venc等模块的初始化，rkmpi、isp等子系统的初始化
int rkvideo_init() {
    LOG_INFO("Initializing rkvideo module...");

    RK_S32 s32Ret = 0; 

	int width    = DISP_WIDTH;
    int height   = DISP_HEIGHT;

    LOG_DEBUG("Display resolution: {}x{}", width, height);

    // rkaiq init 
	RK_BOOL multi_sensor = RK_FALSE;	
	const char *iq_dir = "/etc/iqfiles"; // IQ文件路径
	rk_aiq_working_mode_t hdr_mode = RK_AIQ_WORKING_MODE_NORMAL;
	//hdr_mode = RK_AIQ_WORKING_MODE_ISP_HDR2;
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
	
	// rtsp init	
	// g_rtsplive = create_rtsp_demo(554);
	// g_rtsp_session = rtsp_new_session(g_rtsplive, "/live/0");
	// rtsp_set_video(g_rtsp_session, RTSP_CODEC_ID_VIDEO_H264, NULL, 0);
	// rtsp_sync_video_ts(g_rtsp_session, rtsp_get_reltime(), rtsp_get_ntptime());

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
			
	LOG_INFO("rkvideo module initialized successfully");

    return 0;
}

// rk媒体库的反初始化入口，释放vi、venc等模块的资源，退出rkmpi、isp等子系统
int rkvideo_deinit() {
    LOG_INFO("Deinitializing rkvideo module...");

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