#include "vi.h"

#include <spdlog/spdlog.h>

int vi_dev_init() {
    SPDLOG_DEBUG("{}", __func__);
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
            SPDLOG_ERROR("RK_MPI_VI_SetDevAttr {:x}", static_cast<unsigned int>(ret));
            return -1;
        }
    } else {
        SPDLOG_INFO("RK_MPI_VI_SetDevAttr already");
    }
    // 1.get dev enable status
    ret = RK_MPI_VI_GetDevIsEnable(devId);
    if (ret != RK_SUCCESS) {
        // 1-2.enable dev
        ret = RK_MPI_VI_EnableDev(devId);
        if (ret != RK_SUCCESS) {
            SPDLOG_ERROR("RK_MPI_VI_EnableDev {:x}", static_cast<unsigned int>(ret));
            return -1;
        }
        // 1-3.bind dev/pipe
        stBindPipe.u32Num = 1;
        stBindPipe.PipeId[0] = pipeId;
        ret = RK_MPI_VI_SetDevBindPipe(devId, &stBindPipe);
        if (ret != RK_SUCCESS) {
            SPDLOG_ERROR("RK_MPI_VI_SetDevBindPipe {:x}", static_cast<unsigned int>(ret));
            return -1;
        }
    } else {
        SPDLOG_INFO("RK_MPI_VI_EnableDev already");
    }

    return 0;
}

int vi_chn_init(int channelId, int width, int height) {
    int ret;
    int buf_cnt = 2;
    // VI init
    VI_CHN_ATTR_S vi_chn_attr;
    memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
    vi_chn_attr.stIspOpt.u32BufCount = buf_cnt;
    vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF; // VI_V4L2_MEMORY_TYPE_MMAP;
    vi_chn_attr.stSize.u32Width = width;
    vi_chn_attr.stSize.u32Height = height;
    vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
    vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE; // COMPRESS_AFBC_16x16;
    vi_chn_attr.u32Depth = 2; // 0, get fail, 1 - u32BufCount, can get, if bind to other device, must be < u32BufCount
    ret = RK_MPI_VI_SetChnAttr(0, channelId, &vi_chn_attr);
    ret |= RK_MPI_VI_EnableChn(0, channelId);
    if (ret) {
        SPDLOG_ERROR("create VI error ret={}", ret);
        return ret;
    }

    return ret;
}
