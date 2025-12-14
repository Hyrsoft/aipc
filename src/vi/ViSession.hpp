#pragma once

#include <cstring>

#include <spdlog/spdlog.h>

#include "sample_comm.h"

namespace aipc::vi {

class ViSession {
public:
    ViSession(int dev, int chn, int width, int height) : dev_(dev), chn_(chn) {
        if (init_dev() != 0) {
            SPDLOG_ERROR("VI init_dev failed");
            ok_ = false;
            return;
        }
        const int ret = init_chn(width, height);
        if (ret != 0) {
            SPDLOG_ERROR("VI init_chn failed ret={}", ret);
            ok_ = false;
            return;
        }
        ok_ = true;
    }

    ~ViSession() {
        if (chn_enabled_) {
            const auto ret = RK_MPI_VI_DisableChn(dev_, chn_);
            if (ret != RK_SUCCESS) {
                SPDLOG_WARN("RK_MPI_VI_DisableChn failed {:x}", static_cast<unsigned int>(ret));
            }
            chn_enabled_ = false;
        }

        if (dev_enabled_) {
            const auto ret = RK_MPI_VI_DisableDev(dev_);
            if (ret != RK_SUCCESS) {
                SPDLOG_WARN("RK_MPI_VI_DisableDev failed {:x}", static_cast<unsigned int>(ret));
            }
            dev_enabled_ = false;
        }
    }

    ViSession(const ViSession &) = delete;
    ViSession &operator=(const ViSession &) = delete;
    ViSession(ViSession &&) = delete;
    ViSession &operator=(ViSession &&) = delete;

    bool ok() const { return ok_; }

private:
    int init_dev() {
        VI_DEV_ATTR_S dev_attr;
        std::memset(&dev_attr, 0, sizeof(dev_attr));

        // 0) Configure device if needed.
        auto ret = RK_MPI_VI_GetDevAttr(dev_, &dev_attr);
        if (ret == RK_ERR_VI_NOT_CONFIG) {
            ret = RK_MPI_VI_SetDevAttr(dev_, &dev_attr);
            if (ret != RK_SUCCESS) {
                SPDLOG_ERROR("RK_MPI_VI_SetDevAttr failed {:x}", static_cast<unsigned int>(ret));
                return -1;
            }
        }

        // 1) Enable device if not already enabled.
        ret = RK_MPI_VI_GetDevIsEnable(dev_);
        if (ret != RK_SUCCESS) {
            ret = RK_MPI_VI_EnableDev(dev_);
            if (ret != RK_SUCCESS) {
                SPDLOG_ERROR("RK_MPI_VI_EnableDev failed {:x}", static_cast<unsigned int>(ret));
                return -1;
            }
            dev_enabled_ = true;

            VI_DEV_BIND_PIPE_S bind_pipe;
            std::memset(&bind_pipe, 0, sizeof(bind_pipe));
            bind_pipe.u32Num = 1;
            bind_pipe.PipeId[0] = dev_;
            ret = RK_MPI_VI_SetDevBindPipe(dev_, &bind_pipe);
            if (ret != RK_SUCCESS) {
                SPDLOG_ERROR("RK_MPI_VI_SetDevBindPipe failed {:x}", static_cast<unsigned int>(ret));
                return -1;
            }
        } else {
            dev_enabled_ = true;
        }

        return 0;
    }

    int init_chn(int width, int height) {
        constexpr int buf_count = 2;

        VI_CHN_ATTR_S chn_attr;
        std::memset(&chn_attr, 0, sizeof(chn_attr));
        chn_attr.stIspOpt.u32BufCount = buf_count;
        chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
        chn_attr.stSize.u32Width = width;
        chn_attr.stSize.u32Height = height;
        chn_attr.enPixelFormat = RK_FMT_YUV420SP;
        chn_attr.enCompressMode = COMPRESS_MODE_NONE;
        chn_attr.u32Depth = 2;

        auto ret = RK_MPI_VI_SetChnAttr(dev_, chn_, &chn_attr);
        if (ret != RK_SUCCESS) {
            SPDLOG_ERROR("RK_MPI_VI_SetChnAttr failed {:x}", static_cast<unsigned int>(ret));
            return -1;
        }

        ret = RK_MPI_VI_EnableChn(dev_, chn_);
        if (ret != RK_SUCCESS) {
            SPDLOG_ERROR("RK_MPI_VI_EnableChn failed {:x}", static_cast<unsigned int>(ret));
            return -1;
        }
        chn_enabled_ = true;
        return 0;
    }

    int dev_{0};
    int chn_{0};
    bool ok_{false};
    bool dev_enabled_{false};
    bool chn_enabled_{false};
};

} // namespace aipc::vi
