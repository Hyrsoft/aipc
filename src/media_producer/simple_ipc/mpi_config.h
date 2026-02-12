/**
 * @file mpi_config.h
 * @brief MPI 配置常量和辅助函数
 *
 * 包含各生产者共用的 VI/VPSS/VENC 通道 ID 定义和 MPI 初始化函数。
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#pragma once

#include "sample_comm.h"
#include "rk_mpi_vpss.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"

namespace media {

// ============================================================================
// MPI 通道/Group 常量
// ============================================================================

constexpr int kViDev = 0;       ///< VI 设备 ID
constexpr int kViChn = 0;       ///< VI 通道 ID
constexpr int kVpssGrp = 0;     ///< VPSS Group ID
constexpr int kVpssChn0 = 0;    ///< VPSS 通道 0（编码流）
constexpr int kVpssChn1 = 1;    ///< VPSS 通道 1（AI 推理）
constexpr int kVencChn = 0;     ///< VENC 通道 ID

// ============================================================================
// VI 初始化函数
// ============================================================================

/**
 * @brief 初始化 VI 设备
 * 
 * RV1106 SDK 简化版：仅 memset 后调用 API
 */
inline int vi_dev_init() {
    int ret = 0;
    int devId = kViDev;
    int pipeId = devId;

    VI_DEV_ATTR_S stDevAttr;
    VI_DEV_BIND_PIPE_S stBindPipe;
    memset(&stDevAttr, 0, sizeof(stDevAttr));
    memset(&stBindPipe, 0, sizeof(stBindPipe));

    // 检查设备配置状态
    ret = RK_MPI_VI_GetDevAttr(devId, &stDevAttr);
    if (ret == RK_ERR_VI_NOT_CONFIG) {
        ret = RK_MPI_VI_SetDevAttr(devId, &stDevAttr);
        if (ret != RK_SUCCESS) {
            return -1;
        }
    }

    // 检查设备启用状态
    ret = RK_MPI_VI_GetDevIsEnable(devId);
    if (ret != RK_SUCCESS) {
        ret = RK_MPI_VI_EnableDev(devId);
        if (ret != RK_SUCCESS) {
            return -1;
        }
        stBindPipe.u32Num = 1;
        stBindPipe.PipeId[0] = pipeId;
        ret = RK_MPI_VI_SetDevBindPipe(devId, &stBindPipe);
        if (ret != RK_SUCCESS) {
            return -1;
        }
    }

    return 0;
}

/**
 * @brief 初始化 VI 通道
 */
inline int vi_chn_init(int channelId, int width, int height) {
    VI_CHN_ATTR_S stChnAttr;
    memset(&stChnAttr, 0, sizeof(stChnAttr));
    stChnAttr.stIspOpt.stMaxSize.u32Width = width;
    stChnAttr.stIspOpt.stMaxSize.u32Height = height;
    stChnAttr.stIspOpt.u32BufCount = 2;
    stChnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    stChnAttr.stSize.u32Width = width;
    stChnAttr.stSize.u32Height = height;
    stChnAttr.enPixelFormat = RK_FMT_YUV420SP;
    stChnAttr.enCompressMode = COMPRESS_MODE_NONE;
    stChnAttr.u32Depth = 0;

    RK_MPI_VI_SetChnAttr(kViDev, channelId, &stChnAttr);
    RK_MPI_VI_EnableChn(kViDev, channelId);
    return 0;
}

// ============================================================================
// VPSS 初始化函数
// ============================================================================

/**
 * @brief 初始化 VPSS Group 和通道（并行模式）
 * 
 * 并行模式：Chn0 的 u32Depth = 0，硬件绑定到 VENC
 */
inline int vpss_init(int grpId, int inputWidth, int inputHeight,
                     int chn0Width, int chn0Height,
                     int chn1Width = 0, int chn1Height = 0) {
    // 创建 Group
    VPSS_GRP_ATTR_S stGrpAttr;
    memset(&stGrpAttr, 0, sizeof(stGrpAttr));
    stGrpAttr.u32MaxW = inputWidth;
    stGrpAttr.u32MaxH = inputHeight;
    stGrpAttr.enPixelFormat = RK_FMT_YUV420SP;
    stGrpAttr.stFrameRate.s32SrcFrameRate = -1;
    stGrpAttr.stFrameRate.s32DstFrameRate = -1;
    stGrpAttr.enCompressMode = COMPRESS_MODE_NONE;

    RK_S32 ret = RK_MPI_VPSS_CreateGrp(grpId, &stGrpAttr);
    if (ret != RK_SUCCESS) {
        return -1;
    }

    // Chn0: 编码流
    VPSS_CHN_ATTR_S stChnAttr;
    memset(&stChnAttr, 0, sizeof(stChnAttr));
    stChnAttr.enChnMode = VPSS_CHN_MODE_USER;
    stChnAttr.enDynamicRange = DYNAMIC_RANGE_SDR8;
    stChnAttr.enPixelFormat = RK_FMT_YUV420SP;
    stChnAttr.stFrameRate.s32SrcFrameRate = -1;
    stChnAttr.stFrameRate.s32DstFrameRate = -1;
    stChnAttr.u32Width = chn0Width;
    stChnAttr.u32Height = chn0Height;
    stChnAttr.u32Depth = 0;  // 硬件绑定，不保存帧
    stChnAttr.enCompressMode = COMPRESS_MODE_NONE;

    RK_MPI_VPSS_SetChnAttr(grpId, kVpssChn0, &stChnAttr);
    RK_MPI_VPSS_EnableChn(grpId, kVpssChn0);

    // Chn1: AI 推理（可选）
    if (chn1Width > 0 && chn1Height > 0) {
        memset(&stChnAttr, 0, sizeof(stChnAttr));
        stChnAttr.enChnMode = VPSS_CHN_MODE_USER;
        stChnAttr.enDynamicRange = DYNAMIC_RANGE_SDR8;
        stChnAttr.enPixelFormat = RK_FMT_YUV420SP;
        stChnAttr.stFrameRate.s32SrcFrameRate = -1;
        stChnAttr.stFrameRate.s32DstFrameRate = -1;
        stChnAttr.u32Width = chn1Width;
        stChnAttr.u32Height = chn1Height;
        stChnAttr.u32Depth = 2;  // 允许手动获取
        stChnAttr.enCompressMode = COMPRESS_MODE_NONE;

        RK_MPI_VPSS_SetChnAttr(grpId, kVpssChn1, &stChnAttr);
        RK_MPI_VPSS_EnableChn(grpId, kVpssChn1);
    }

    RK_MPI_VPSS_StartGrp(grpId);
    return 0;
}

/**
 * @brief 初始化 VPSS（串行模式）
 * 
 * 串行模式：Chn0 的 u32Depth > 0，允许手动 GetChnFrame
 */
inline int vpss_init_serial_mode(int grpId, int width, int height) {
    // 创建 Group
    VPSS_GRP_ATTR_S stGrpAttr;
    memset(&stGrpAttr, 0, sizeof(stGrpAttr));
    stGrpAttr.u32MaxW = width;
    stGrpAttr.u32MaxH = height;
    stGrpAttr.enPixelFormat = RK_FMT_YUV420SP;
    stGrpAttr.stFrameRate.s32SrcFrameRate = -1;
    stGrpAttr.stFrameRate.s32DstFrameRate = -1;
    stGrpAttr.enCompressMode = COMPRESS_MODE_NONE;

    RK_S32 ret = RK_MPI_VPSS_CreateGrp(grpId, &stGrpAttr);
    if (ret != RK_SUCCESS) {
        return -1;
    }

    // Chn0: 手动获取帧
    VPSS_CHN_ATTR_S stChnAttr;
    memset(&stChnAttr, 0, sizeof(stChnAttr));
    stChnAttr.enChnMode = VPSS_CHN_MODE_USER;
    stChnAttr.enDynamicRange = DYNAMIC_RANGE_SDR8;
    stChnAttr.enPixelFormat = RK_FMT_YUV420SP;
    stChnAttr.stFrameRate.s32SrcFrameRate = -1;
    stChnAttr.stFrameRate.s32DstFrameRate = -1;
    stChnAttr.u32Width = width;
    stChnAttr.u32Height = height;
    stChnAttr.u32Depth = 2;  // 允许手动获取
    stChnAttr.enCompressMode = COMPRESS_MODE_NONE;

    RK_MPI_VPSS_SetChnAttr(grpId, kVpssChn0, &stChnAttr);
    RK_MPI_VPSS_EnableChn(grpId, kVpssChn0);

    RK_MPI_VPSS_StartGrp(grpId);
    return 0;
}

/**
 * @brief 销毁 VPSS Group 和通道
 */
inline int vpss_deinit(int grpId, bool enableChn1 = false) {
    RK_MPI_VPSS_StopGrp(grpId);
    RK_MPI_VPSS_DisableChn(grpId, kVpssChn0);
    if (enableChn1) {
        RK_MPI_VPSS_DisableChn(grpId, kVpssChn1);
    }
    RK_MPI_VPSS_DestroyGrp(grpId);
    return 0;
}

// ============================================================================
// VENC 初始化函数
// ============================================================================

/**
 * @brief 初始化 VENC（NV12 输入）
 */
inline int venc_init(int chnId, int width, int height, RK_CODEC_ID_E enType) {
    VENC_CHN_ATTR_S stAttr;
    memset(&stAttr, 0, sizeof(stAttr));

    stAttr.stVencAttr.enType = enType;
    stAttr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    stAttr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
    stAttr.stVencAttr.u32PicWidth = width;
    stAttr.stVencAttr.u32PicHeight = height;
    stAttr.stVencAttr.u32VirWidth = width;
    stAttr.stVencAttr.u32VirHeight = height;
    stAttr.stVencAttr.u32StreamBufCnt = 2;
    stAttr.stVencAttr.u32BufSize = width * height / 2;

    stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    stAttr.stRcAttr.stH264Cbr.u32Gop = 30;
    stAttr.stRcAttr.stH264Cbr.u32BitRate = 10 * 1024;  // 10 Mbps
    stAttr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
    stAttr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = 30;
    stAttr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
    stAttr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = 30;

    RK_MPI_VENC_CreateChn(chnId, &stAttr);

    VENC_RECV_PIC_PARAM_S stRecvParam;
    memset(&stRecvParam, 0, sizeof(stRecvParam));
    stRecvParam.s32RecvPicNum = -1;
    RK_MPI_VENC_StartRecvFrame(chnId, &stRecvParam);

    return 0;
}

/**
 * @brief 初始化 VENC（RGB888 输入，用于串行模式）
 */
inline int venc_init_rgb_input(int chnId, int width, int height, RK_CODEC_ID_E enType) {
    VENC_CHN_ATTR_S stAttr;
    memset(&stAttr, 0, sizeof(stAttr));

    stAttr.stVencAttr.enType = enType;
    stAttr.stVencAttr.enPixelFormat = RK_FMT_RGB888;  // RGB 输入
    stAttr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
    stAttr.stVencAttr.u32PicWidth = width;
    stAttr.stVencAttr.u32PicHeight = height;
    stAttr.stVencAttr.u32VirWidth = width;
    stAttr.stVencAttr.u32VirHeight = height;
    stAttr.stVencAttr.u32StreamBufCnt = 2;
    stAttr.stVencAttr.u32BufSize = width * height;

    stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    stAttr.stRcAttr.stH264Cbr.u32Gop = 30;
    stAttr.stRcAttr.stH264Cbr.u32BitRate = 8 * 1024;  // 8 Mbps
    stAttr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
    stAttr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = 30;
    stAttr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
    stAttr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = 30;

    RK_MPI_VENC_CreateChn(chnId, &stAttr);

    VENC_RECV_PIC_PARAM_S stRecvParam;
    memset(&stRecvParam, 0, sizeof(stRecvParam));
    stRecvParam.s32RecvPicNum = -1;
    RK_MPI_VENC_StartRecvFrame(chnId, &stRecvParam);

    return 0;
}

}  // namespace media
