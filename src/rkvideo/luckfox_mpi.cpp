/*****************************************************************************
* | Author      :   Luckfox team
* | Function    :   
* | Info        :
*
*----------------
* | This version:   V1.1
* | Date        :   2024-08-26
* | Info        :   Basic version
*
******************************************************************************/

#include "luckfox_mpi.h"

RK_U64 TEST_COMM_GetNowUs() {
	struct timespec time = {0, 0};
	clock_gettime(CLOCK_MONOTONIC, &time);
	return (RK_U64)time.tv_sec * 1000000 + (RK_U64)time.tv_nsec / 1000; /* microseconds */
}

int vi_dev_init() {
	printf("%s\n", __func__);
	int ret = 0;
	int devId = 0;
	int pipeId = devId;

	VI_DEV_ATTR_S stDevAttr;
	VI_DEV_BIND_PIPE_S stBindPipe;
	memset(&stDevAttr, 0, sizeof(stDevAttr));
	memset(&stBindPipe, 0, sizeof(stBindPipe));
	// 0. get dev config status
	ret = RK_MPI_VI_GetDevAttr(devId, &stDevAttr);
	if (ret == RK_ERR_VI_NOT_CONFIG) {
		// 0-1.config dev
		ret = RK_MPI_VI_SetDevAttr(devId, &stDevAttr);
		if (ret != RK_SUCCESS) {
			printf("RK_MPI_VI_SetDevAttr %x\n", ret);
			return -1;
		}
	} else {
		printf("RK_MPI_VI_SetDevAttr already\n");
	}
	// 1.get dev enable status
	ret = RK_MPI_VI_GetDevIsEnable(devId);
	if (ret != RK_SUCCESS) {
		// 1-2.enable dev
		ret = RK_MPI_VI_EnableDev(devId);
		if (ret != RK_SUCCESS) {
			printf("RK_MPI_VI_EnableDev %x\n", ret);
			return -1;
		}
		// 1-3.bind dev/pipe
		stBindPipe.u32Num = 1;
		stBindPipe.PipeId[0] = pipeId;
		ret = RK_MPI_VI_SetDevBindPipe(devId, &stBindPipe);
		if (ret != RK_SUCCESS) {
			printf("RK_MPI_VI_SetDevBindPipe %x\n", ret);
			return -1;
		}
	} else {
		printf("RK_MPI_VI_EnableDev already\n");
	}

	return 0;
}

int vi_chn_init(int channelId, int width, int height) {
	int ret;
	int buf_cnt = 4;  // 增加 buffer 数量
	// VI init
	VI_CHN_ATTR_S vi_chn_attr;
	memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
	vi_chn_attr.stIspOpt.u32BufCount = buf_cnt;
	vi_chn_attr.stIspOpt.enMemoryType =
	    VI_V4L2_MEMORY_TYPE_DMABUF; // VI_V4L2_MEMORY_TYPE_MMAP;
	vi_chn_attr.stSize.u32Width = width;
	vi_chn_attr.stSize.u32Height = height;
	vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
	vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE; // COMPRESS_AFBC_16x16;
	// 当 VI 绑定到 VPSS 时，u32Depth 必须为 0（完全绑定模式）
	vi_chn_attr.u32Depth = 0;
	ret = RK_MPI_VI_SetChnAttr(0, channelId, &vi_chn_attr);
	ret |= RK_MPI_VI_EnableChn(0, channelId);
	if (ret) {
		printf("ERROR: create VI error! ret=%d\n", ret);
		return ret;
	}

	return ret;
}

int vpss_init(int grpId, int inputWidth, int inputHeight,
              int chn0Width, int chn0Height,
              int chn1Width, int chn1Height) {
	printf("%s: grp=%d, input=%dx%d, chn0=%dx%d, chn1=%dx%d\n", 
	       __func__, grpId, inputWidth, inputHeight, 
	       chn0Width, chn0Height, chn1Width, chn1Height);
	
	int ret;
	
	// ==================== VPSS Group 配置 ====================
	VPSS_GRP_ATTR_S stVpssGrpAttr;
	memset(&stVpssGrpAttr, 0, sizeof(VPSS_GRP_ATTR_S));
	
	stVpssGrpAttr.u32MaxW = inputWidth;
	stVpssGrpAttr.u32MaxH = inputHeight;
	stVpssGrpAttr.enPixelFormat = RK_FMT_YUV420SP;
	stVpssGrpAttr.enCompressMode = COMPRESS_MODE_NONE;
	stVpssGrpAttr.stFrameRate.s32SrcFrameRate = -1;
	stVpssGrpAttr.stFrameRate.s32DstFrameRate = -1;
	
	ret = RK_MPI_VPSS_CreateGrp(grpId, &stVpssGrpAttr);
	if (ret != RK_SUCCESS) {
		printf("ERROR: RK_MPI_VPSS_CreateGrp failed! ret=0x%x\n", ret);
		return -1;
	}
	
	// ==================== VPSS Chn0 配置（给 VENC）====================
	// u32Depth = 0: 绑定模式，帧直接送到下游模块（VENC）
	VPSS_CHN_ATTR_S stVpssChn0Attr;
	memset(&stVpssChn0Attr, 0, sizeof(VPSS_CHN_ATTR_S));
	
	stVpssChn0Attr.enChnMode = VPSS_CHN_MODE_USER;
	stVpssChn0Attr.enCompressMode = COMPRESS_MODE_NONE;
	stVpssChn0Attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
	stVpssChn0Attr.enPixelFormat = RK_FMT_YUV420SP;
	stVpssChn0Attr.stFrameRate.s32SrcFrameRate = -1;
	stVpssChn0Attr.stFrameRate.s32DstFrameRate = -1;
	stVpssChn0Attr.u32Width = chn0Width;
	stVpssChn0Attr.u32Height = chn0Height;
	stVpssChn0Attr.u32Depth = 0;  // 绑定模式
	stVpssChn0Attr.bFlip = RK_FALSE;
	stVpssChn0Attr.bMirror = RK_FALSE;
	stVpssChn0Attr.u32FrameBufCnt = 2;
	
	ret = RK_MPI_VPSS_SetChnAttr(grpId, VPSS_CHN0, &stVpssChn0Attr);
	if (ret != RK_SUCCESS) {
		printf("ERROR: RK_MPI_VPSS_SetChnAttr Chn0 failed! ret=0x%x\n", ret);
		goto destroy_grp;
	}
	
	ret = RK_MPI_VPSS_EnableChn(grpId, VPSS_CHN0);
	if (ret != RK_SUCCESS) {
		printf("ERROR: RK_MPI_VPSS_EnableChn Chn0 failed! ret=0x%x\n", ret);
		goto destroy_grp;
	}
	
	// ==================== VPSS Chn1 配置（给 AI 推理，可选）====================
	// u32Depth > 0: 用户模式，可以通过 GetChnFrame 获取帧
	if (chn1Width > 0 && chn1Height > 0) {
		VPSS_CHN_ATTR_S stVpssChn1Attr;
		memset(&stVpssChn1Attr, 0, sizeof(VPSS_CHN_ATTR_S));
		
		stVpssChn1Attr.enChnMode = VPSS_CHN_MODE_USER;
		stVpssChn1Attr.enCompressMode = COMPRESS_MODE_NONE;
		stVpssChn1Attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
		stVpssChn1Attr.enPixelFormat = RK_FMT_YUV420SP;
		stVpssChn1Attr.stFrameRate.s32SrcFrameRate = -1;
		stVpssChn1Attr.stFrameRate.s32DstFrameRate = -1;
		stVpssChn1Attr.u32Width = chn1Width;
		stVpssChn1Attr.u32Height = chn1Height;
		stVpssChn1Attr.u32Depth = 2;  // 用户模式，允许 GetChnFrame
		stVpssChn1Attr.bFlip = RK_FALSE;
		stVpssChn1Attr.bMirror = RK_FALSE;
		stVpssChn1Attr.u32FrameBufCnt = 2;
		
		ret = RK_MPI_VPSS_SetChnAttr(grpId, VPSS_CHN1, &stVpssChn1Attr);
		if (ret != RK_SUCCESS) {
			printf("ERROR: RK_MPI_VPSS_SetChnAttr Chn1 failed! ret=0x%x\n", ret);
			goto disable_chn0;
		}
		
		ret = RK_MPI_VPSS_EnableChn(grpId, VPSS_CHN1);
		if (ret != RK_SUCCESS) {
			printf("ERROR: RK_MPI_VPSS_EnableChn Chn1 failed! ret=0x%x\n", ret);
			goto disable_chn0;
		}
	}
	
	// ==================== 启动 VPSS Group ====================
	ret = RK_MPI_VPSS_StartGrp(grpId);
	if (ret != RK_SUCCESS) {
		printf("ERROR: RK_MPI_VPSS_StartGrp failed! ret=0x%x\n", ret);
		goto disable_chns;
	}
	
	printf("VPSS init success!\n");
	return 0;
	
disable_chns:
	if (chn1Width > 0 && chn1Height > 0) {
		RK_MPI_VPSS_DisableChn(grpId, VPSS_CHN1);
	}
disable_chn0:
	RK_MPI_VPSS_DisableChn(grpId, VPSS_CHN0);
destroy_grp:
	RK_MPI_VPSS_DestroyGrp(grpId);
	return -1;
}

int vpss_deinit(int grpId, bool enableChn1) {
	printf("%s: grp=%d, enableChn1=%d\n", __func__, grpId, enableChn1);
	
	RK_MPI_VPSS_StopGrp(grpId);
	
	if (enableChn1) {
		RK_MPI_VPSS_DisableChn(grpId, VPSS_CHN1);
	}
	RK_MPI_VPSS_DisableChn(grpId, VPSS_CHN0);
	
	RK_MPI_VPSS_DestroyGrp(grpId);
	
	return 0;
}

int vpss_reconfigure_chn1(int grpId, int width, int height) {
	printf("%s: grp=%d, new_size=%dx%d\n", __func__, grpId, width, height);

	int ret;

	// 1. 先禁用 Chn1
	ret = RK_MPI_VPSS_DisableChn(grpId, VPSS_CHN1);
	if (ret != RK_SUCCESS) {
		printf("ERROR: RK_MPI_VPSS_DisableChn Chn1 failed! ret=0x%x\n", ret);
		return -1;
	}

	// 2. 重新配置 Chn1 属性
	VPSS_CHN_ATTR_S stVpssChn1Attr;
	memset(&stVpssChn1Attr, 0, sizeof(VPSS_CHN_ATTR_S));

	stVpssChn1Attr.enChnMode = VPSS_CHN_MODE_USER;
	stVpssChn1Attr.enCompressMode = COMPRESS_MODE_NONE;
	stVpssChn1Attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
	stVpssChn1Attr.enPixelFormat = RK_FMT_YUV420SP;
	stVpssChn1Attr.stFrameRate.s32SrcFrameRate = -1;
	stVpssChn1Attr.stFrameRate.s32DstFrameRate = -1;
	stVpssChn1Attr.u32Width = width;
	stVpssChn1Attr.u32Height = height;
	stVpssChn1Attr.u32Depth = 2;  // 用户模式，允许 GetChnFrame
	stVpssChn1Attr.bFlip = RK_FALSE;
	stVpssChn1Attr.bMirror = RK_FALSE;
	stVpssChn1Attr.u32FrameBufCnt = 2;

	ret = RK_MPI_VPSS_SetChnAttr(grpId, VPSS_CHN1, &stVpssChn1Attr);
	if (ret != RK_SUCCESS) {
		printf("ERROR: RK_MPI_VPSS_SetChnAttr Chn1 failed! ret=0x%x\n", ret);
		// 尝试恢复通道
		RK_MPI_VPSS_EnableChn(grpId, VPSS_CHN1);
		return -1;
	}

	// 3. 重新启用 Chn1
	ret = RK_MPI_VPSS_EnableChn(grpId, VPSS_CHN1);
	if (ret != RK_SUCCESS) {
		printf("ERROR: RK_MPI_VPSS_EnableChn Chn1 failed! ret=0x%x\n", ret);
		return -1;
	}

	printf("VPSS Chn1 reconfigured to %dx%d\n", width, height);
	return 0;
}

int venc_init(int chnId, int width, int height, RK_CODEC_ID_E enType) {
	printf("%s\n",__func__);
	VENC_RECV_PIC_PARAM_S stRecvParam;
	VENC_CHN_ATTR_S stAttr;
	memset(&stAttr, 0, sizeof(VENC_CHN_ATTR_S));

	if (enType == RK_VIDEO_ID_AVC) {
		stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
		stAttr.stRcAttr.stH264Cbr.u32BitRate = 10 * 1024;

		// stAttr.stRcAttr.stH264Cbr.u32Gop = 1;
		stAttr.stRcAttr.stH264Cbr.u32Gop = 60;  // GOP=60，每2秒一个I帧（30fps）
	} else if (enType == RK_VIDEO_ID_HEVC) {
		stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
		stAttr.stRcAttr.stH265Cbr.u32BitRate = 10 * 1024;
		stAttr.stRcAttr.stH265Cbr.u32Gop = 60;
	} else if (enType == RK_VIDEO_ID_MJPEG) {
		stAttr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGCBR;
		stAttr.stRcAttr.stMjpegCbr.u32BitRate = 10 * 1024;
	}

	stAttr.stVencAttr.enType = enType;
	// stAttr.stVencAttr.enPixelFormat = RK_FMT_RGB888
	stAttr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;  // 与 VI 输出格式匹配
	if (enType == RK_VIDEO_ID_AVC)
		stAttr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
	stAttr.stVencAttr.u32PicWidth = width;
	stAttr.stVencAttr.u32PicHeight = height;
	stAttr.stVencAttr.u32VirWidth = width;
	stAttr.stVencAttr.u32VirHeight = height;
	stAttr.stVencAttr.u32StreamBufCnt = 4;  // 增加 buffer 数量，避免耗尽
	stAttr.stVencAttr.u32BufSize = width * height * 3 / 2;
	stAttr.stVencAttr.enMirror = MIRROR_NONE;

	RK_MPI_VENC_CreateChn(chnId, &stAttr);

	memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
	stRecvParam.s32RecvPicNum = -1;
	RK_MPI_VENC_StartRecvFrame(chnId, &stRecvParam);

	return 0;
}