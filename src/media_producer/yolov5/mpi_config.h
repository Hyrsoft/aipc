/**
 * @file mpi_config.h
 * @brief YOLOv5 模式专用 MPI 配置
 *
 * 对齐 luckfox_pico_rtsp_yolov5 例程：
 * - VI (u32Depth=2, 手动 GetChnFrame, 无 VPSS)
 * - VENC (RGB888 输入, 手动 SendFrame)
 *
 * 每个模式完全隔离：InitMpi 先强制清理残留状态再重新配置
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#pragma once

#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"
#include "sample_comm.h"

#include <cstring>

namespace media {

    // ============================================================================
    // MPI 通道/Group 常量
    // ============================================================================

    constexpr int kViDev = 0; ///< VI 设备 ID
    constexpr int kViChn = 0; ///< VI 通道 ID
    constexpr int kVencChn = 0; ///< VENC 通道 ID

    // ============================================================================
    // YOLOv5 模式默认参数
    // ============================================================================

    constexpr int kDefaultAiWidth = 640;
    constexpr int kDefaultAiHeight = 640;
    constexpr float kDefaultConfThreshold = 0.25f;
    constexpr float kDefaultNmsThreshold = 0.45f;
    constexpr const char *kDefaultModelPath = "../model/yolov5.rknn";
    constexpr const char *kDefaultLabelsPath = "../model/coco_80_labels_list.txt";

    // ============================================================================
    // 强制清理函数
    // ============================================================================

    /**
     * @brief 强制清理 VI/VENC 残留状态
     *
     * 模式切换时，前一个模式的 deinit 可能留下某些残留状态。
     * 在 InitMpi 开头调用此函数，确保从干净状态开始。
     * 忽略所有返回值（残留可能不存在）。
     */
    inline void force_cleanup_mpi_state() {
        // 先尝试解绑可能残留的 VI 绑定
        MPP_CHN_S vi_src;
        vi_src.enModId = RK_ID_VI;
        vi_src.s32DevId = kViDev;
        vi_src.s32ChnId = kViChn;

        // 尝试解绑所有可能的下游模块
        MPP_CHN_S vpss_dst;
        vpss_dst.enModId = RK_ID_VPSS;
        vpss_dst.s32DevId = 0;
        vpss_dst.s32ChnId = 0;
        RK_MPI_SYS_UnBind(&vi_src, &vpss_dst); // 忽略错误

        MPP_CHN_S venc_dst;
        venc_dst.enModId = RK_ID_VENC;
        venc_dst.s32DevId = 0;
        venc_dst.s32ChnId = kVencChn;
        RK_MPI_SYS_UnBind(&vi_src, &venc_dst); // 忽略错误

        // 强制禁用 VI 通道和设备
        RK_MPI_VI_DisableChn(kViDev, kViChn); // 忽略错误
        RK_MPI_VI_DisableDev(kViDev); // 忽略错误

        // 强制销毁 VENC
        RK_MPI_VENC_StopRecvFrame(kVencChn); // 忽略错误
        RK_MPI_VENC_DestroyChn(kVencChn); // 忽略错误
    }

    // ============================================================================
    // VI 初始化函数（对齐例程 luckfox_mpi.cc）
    // ============================================================================

    /**
     * @brief 初始化 VI 设备
     */
    inline int vi_dev_init() {
        int ret = 0;
        int devId = kViDev;
        int pipeId = devId;

        VI_DEV_ATTR_S stDevAttr;
        VI_DEV_BIND_PIPE_S stBindPipe;
        memset(&stDevAttr, 0, sizeof(stDevAttr));
        memset(&stBindPipe, 0, sizeof(stBindPipe));

        ret = RK_MPI_VI_GetDevAttr(devId, &stDevAttr);
        if (ret == RK_ERR_VI_NOT_CONFIG) {
            ret = RK_MPI_VI_SetDevAttr(devId, &stDevAttr);
            if (ret != RK_SUCCESS) {
                return -1;
            }
        }

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
     *
     * 与例程对齐：u32Depth = 2，支持手动 RK_MPI_VI_GetChnFrame
     */
    inline int vi_chn_init(int channelId, int width, int height) {
        int ret;
        int buf_cnt = 2;

        VI_CHN_ATTR_S vi_chn_attr;
        memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
        vi_chn_attr.stIspOpt.stMaxSize.u32Width = width;
        vi_chn_attr.stIspOpt.stMaxSize.u32Height = height;
        vi_chn_attr.stIspOpt.u32BufCount = buf_cnt;
        vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
        vi_chn_attr.stSize.u32Width = width;
        vi_chn_attr.stSize.u32Height = height;
        vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
        vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE;
        vi_chn_attr.u32Depth = 2; // 支持手动 GetChnFrame（与例程一致）

        ret = RK_MPI_VI_SetChnAttr(kViDev, channelId, &vi_chn_attr);
        ret |= RK_MPI_VI_EnableChn(kViDev, channelId);
        if (ret) {
            return ret;
        }
        return ret;
    }

    // ============================================================================
    // VENC 初始化函数（对齐例程 luckfox_mpi.cc 的 venc_init）
    // ============================================================================

    /**
     * @brief 初始化 VENC（RGB888 输入）
     *
     * 与例程对齐：
     * - enPixelFormat = RK_FMT_RGB888
     * - u32Gop = 1
     * - u32BufSize = width * height * 3 / 2
     */
    inline int venc_init(int chnId, int width, int height, RK_CODEC_ID_E enType = RK_VIDEO_ID_AVC) {
        VENC_CHN_ATTR_S stAttr;
        memset(&stAttr, 0, sizeof(VENC_CHN_ATTR_S));

        if (enType == RK_VIDEO_ID_AVC) {
            stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
            stAttr.stRcAttr.stH264Cbr.u32BitRate = 10 * 1024;
            stAttr.stRcAttr.stH264Cbr.u32Gop = 1;
        } else if (enType == RK_VIDEO_ID_HEVC) {
            stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
            stAttr.stRcAttr.stH265Cbr.u32BitRate = 10 * 1024;
            stAttr.stRcAttr.stH265Cbr.u32Gop = 60;
        } else if (enType == RK_VIDEO_ID_MJPEG) {
            stAttr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGCBR;
            stAttr.stRcAttr.stMjpegCbr.u32BitRate = 10 * 1024;
        }

        stAttr.stVencAttr.enType = enType;
        stAttr.stVencAttr.enPixelFormat = RK_FMT_RGB888;
        if (enType == RK_VIDEO_ID_AVC)
            stAttr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
        stAttr.stVencAttr.u32PicWidth = width;
        stAttr.stVencAttr.u32PicHeight = height;
        stAttr.stVencAttr.u32VirWidth = width;
        stAttr.stVencAttr.u32VirHeight = height;
        stAttr.stVencAttr.u32StreamBufCnt = 2;
        stAttr.stVencAttr.u32BufSize = width * height * 3 / 2;
        stAttr.stVencAttr.enMirror = MIRROR_NONE;

        RK_MPI_VENC_CreateChn(chnId, &stAttr);

        VENC_RECV_PIC_PARAM_S stRecvParam;
        memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
        stRecvParam.s32RecvPicNum = -1;
        RK_MPI_VENC_StartRecvFrame(chnId, &stRecvParam);

        return 0;
    }

} // namespace media
