/**
 * @file mpi_config.h
 * @brief SimpleIPC 模式专用的 RKMPI 初始化辅助函数
 *
 * 包含 VI/VPSS/VENC 通道 ID 常量和对应的初始化/反初始化内联函数。
 *
 * 注意：
 * - 本文件仅供 SimpleIPCProducer 使用。
 * - SimpleIPC 使用 RKMPI 提供的 API（VI -> VPSS -> VENC 硬件绑定，零拷贝）。
 * - VisionG 模式通过 Python 脚本调用 visiong 自带的 API 完成摄像头初始化，
 *   不使用本文件中的任何函数或常量。
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#pragma once

#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_vpss.h"
#include "sample_comm.h"

namespace media::simple_ipc {

    // ============================================================================
    // MPI 通道/Group 常量
    // ============================================================================

    constexpr int kViDev = 0; ///< VI 设备 ID
    constexpr int kViChn = 0; ///< VI 通道 ID
    constexpr int kVpssGrp = 0; ///< VPSS Group ID
    constexpr int kVpssChn = 0; ///< VPSS 通道（SimpleIPC 只使用单通道，硬件绑定 VENC）
    constexpr int kVencChn = 0; ///< VENC 通道 ID

    // ============================================================================
    // VI 初始化函数
    // ============================================================================

    /**
     * @brief 初始化 VI 设备
     *
     * RV1106 SDK 简化版：memset 后调用 RK_MPI_VI API。
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
     * @brief 初始化 VPSS Group 和单通道（并行模式）
     *
     * 并行模式：kVpssChn 的 u32Depth = 0，由硬件直接绑定到 VENC，零拷贝。
     *
     * @param grpId       VPSS Group ID
     * @param inputWidth  输入（来自 VI）的宽度
     * @param inputHeight 输入的高度
     * @param chnWidth    输出通道（送 VENC）的宽度
     * @param chnHeight   输出通道的高度
     */
    inline int vpss_init(int grpId, int inputWidth, int inputHeight, int chnWidth, int chnHeight) {
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

        // 单通道：编码流（u32Depth = 0，硬件绑定，不保存帧）
        VPSS_CHN_ATTR_S stChnAttr;
        memset(&stChnAttr, 0, sizeof(stChnAttr));
        stChnAttr.enChnMode = VPSS_CHN_MODE_USER;
        stChnAttr.enDynamicRange = DYNAMIC_RANGE_SDR8;
        stChnAttr.enPixelFormat = RK_FMT_YUV420SP;
        stChnAttr.stFrameRate.s32SrcFrameRate = -1;
        stChnAttr.stFrameRate.s32DstFrameRate = -1;
        stChnAttr.u32Width = chnWidth;
        stChnAttr.u32Height = chnHeight;
        stChnAttr.u32Depth = 0; // 硬件绑定，不保存帧
        stChnAttr.enCompressMode = COMPRESS_MODE_NONE;

        RK_MPI_VPSS_SetChnAttr(grpId, kVpssChn, &stChnAttr);
        RK_MPI_VPSS_EnableChn(grpId, kVpssChn);

        RK_MPI_VPSS_StartGrp(grpId);
        return 0;
    }

    /**
     * @brief 销毁 VPSS Group 和通道
     */
    inline int vpss_deinit(int grpId) {
        RK_MPI_VPSS_StopGrp(grpId);
        RK_MPI_VPSS_DisableChn(grpId, kVpssChn);
        RK_MPI_VPSS_DestroyGrp(grpId);
        return 0;
    }

    // ============================================================================
    // VENC 初始化函数
    // ============================================================================

    /**
     * @brief 初始化 VENC（NV12/YUV420SP 输入）
     *
     * SimpleIPC 管线中，VPSS 输出 NV12 格式，硬件直接绑定 VENC，零拷贝。
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
        stAttr.stRcAttr.stH264Cbr.u32BitRate = 10 * 1024; // 10 Mbps
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

} // namespace media::simple_ipc
