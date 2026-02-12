/**
 * @file mpi_config.h
 * @brief RetinaFace 模式专用 MPI 配置
 *
 * 串行模式硬件配置：
 * - VI -> VPSS (绑定)
 * - VPSS -> 手动获取 -> NPU -> 手动发送 -> VENC
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#pragma once

#include "sample_comm.h"
#include "rk_mpi_vpss.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"

#include <cstring>

namespace media {

// ============================================================================
// MPI 通道/Group 常量
// ============================================================================

constexpr int kViDev = 0;       ///< VI 设备 ID
constexpr int kViChn = 0;       ///< VI 通道 ID
constexpr int kVpssGrp = 0;     ///< VPSS Group ID
constexpr int kVpssChn0 = 0;    ///< VPSS 通道 0（手动获取，给 AI 推理）
constexpr int kVencChn = 0;     ///< VENC 通道 ID

// ============================================================================
// RetinaFace 模式默认参数
// ============================================================================

constexpr int kDefaultAiWidth = 640;    ///< 默认 AI 输入宽度
constexpr int kDefaultAiHeight = 640;   ///< 默认 AI 输入高度
constexpr float kDefaultConfThreshold = 0.5f;   ///< 默认置信度阈值（人脸检测通常更高）
constexpr float kDefaultNmsThreshold = 0.4f;    ///< 默认 NMS 阈值
// 模型路径相对于可执行文件目录（bin/），模型安装在 model/ 目录
constexpr const char* kDefaultModelPath = "../model/retinaface.rknn";
constexpr int kLandmarkCount = 5;   ///< 人脸关键点数量（左眼、右眼、鼻子、左嘴角、右嘴角）

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
// VPSS 初始化函数 - 串行模式
// ============================================================================

/**
 * @brief 初始化 VPSS（串行模式）
 * 
 * 串行模式特点：
 * - Chn0 的 u32Depth > 0，允许手动 GetChnFrame
 * - 不绑定到 VENC，由用户手动 SendFrame
 */
inline int vpss_init_serial_mode(int grpId, int width, int height) {
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
    stChnAttr.u32Depth = 2;
    stChnAttr.enCompressMode = COMPRESS_MODE_NONE;

    RK_MPI_VPSS_SetChnAttr(grpId, kVpssChn0, &stChnAttr);
    RK_MPI_VPSS_EnableChn(grpId, kVpssChn0);

    RK_MPI_VPSS_StartGrp(grpId);
    return 0;
}

/**
 * @brief 销毁 VPSS Group
 */
inline int vpss_deinit(int grpId, bool enableChn1 = false) {
    RK_MPI_VPSS_StopGrp(grpId);
    RK_MPI_VPSS_DisableChn(grpId, kVpssChn0);
    if (enableChn1) {
        RK_MPI_VPSS_DisableChn(grpId, 1);
    }
    RK_MPI_VPSS_DestroyGrp(grpId);
    return 0;
}

// ============================================================================
// VENC 初始化函数 - RGB 输入模式
// ============================================================================

/**
 * @brief 初始化 VENC（RGB888 输入）
 */
inline int venc_init_rgb_input(int chnId, int width, int height, RK_CODEC_ID_E enType = RK_VIDEO_ID_AVC) {
    VENC_CHN_ATTR_S stAttr;
    memset(&stAttr, 0, sizeof(stAttr));

    stAttr.stVencAttr.enType = enType;
    stAttr.stVencAttr.enPixelFormat = RK_FMT_RGB888;
    stAttr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
    stAttr.stVencAttr.u32PicWidth = width;
    stAttr.stVencAttr.u32PicHeight = height;
    stAttr.stVencAttr.u32VirWidth = width;
    stAttr.stVencAttr.u32VirHeight = height;
    stAttr.stVencAttr.u32StreamBufCnt = 2;
    stAttr.stVencAttr.u32BufSize = width * height;

    stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    stAttr.stRcAttr.stH264Cbr.u32Gop = 30;
    stAttr.stRcAttr.stH264Cbr.u32BitRate = 8 * 1024;
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

// ============================================================================
// 人脸关键点绘制颜色
// ============================================================================

constexpr uint32_t kFaceBoxColor = 0x00FF00FF;      ///< 人脸框：绿色
constexpr uint32_t kLeftEyeColor = 0x0000FFFF;      ///< 左眼：蓝色
constexpr uint32_t kRightEyeColor = 0xFF0000FF;     ///< 右眼：红色
constexpr uint32_t kNoseColor = 0xFFFF00FF;         ///< 鼻子：黄色
constexpr uint32_t kMouthColor = 0xFF00FFFF;        ///< 嘴角：紫色

}  // namespace media
