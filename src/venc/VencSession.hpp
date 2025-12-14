#pragma once

#include <cstring>

#include <spdlog/spdlog.h>

#include "sample_comm.h"

namespace aipc::venc {

class VencSession {
public:
    VencSession(int chn, int width, int height, RK_CODEC_ID_E codec) : chn_(chn) {
        const int ret = Init(width, height, codec);
        if (ret != 0) {
            SPDLOG_ERROR("VENC Init failed ret={}", ret);
            ok_ = false;
            return;
        }
        ok_ = true;
    }

    ~VencSession() {
        if (recv_started_) {
            const auto ret = RK_MPI_VENC_StopRecvFrame(chn_);
            if (ret != RK_SUCCESS) {
                SPDLOG_WARN("RK_MPI_VENC_StopRecvFrame failed {:x}", static_cast<unsigned int>(ret));
            }
            recv_started_ = false;
        }

        if (chn_created_) {
            const auto ret = RK_MPI_VENC_DestroyChn(chn_);
            if (ret != RK_SUCCESS) {
                SPDLOG_WARN("RK_MPI_VENC_DestroyChn failed {:x}", static_cast<unsigned int>(ret));
            }
            chn_created_ = false;
        }
    }

    VencSession(const VencSession &) = delete;
    VencSession &operator=(const VencSession &) = delete;
    VencSession(VencSession &&) = delete;
    VencSession &operator=(VencSession &&) = delete;

    bool ok() const { return ok_; }

private:
    int Init(int width, int height, RK_CODEC_ID_E codec) {
        VENC_CHN_ATTR_S attr;
        std::memset(&attr, 0, sizeof(attr));

        if (codec == RK_VIDEO_ID_AVC) {
            attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
            attr.stRcAttr.stH264Cbr.u32BitRate = 10 * 1024;
            attr.stRcAttr.stH264Cbr.u32Gop = 1;
        } else if (codec == RK_VIDEO_ID_HEVC) {
            attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
            attr.stRcAttr.stH265Cbr.u32BitRate = 10 * 1024;
            attr.stRcAttr.stH265Cbr.u32Gop = 60;
        } else if (codec == RK_VIDEO_ID_MJPEG) {
            attr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGCBR;
            attr.stRcAttr.stMjpegCbr.u32BitRate = 10 * 1024;
        } else {
            SPDLOG_ERROR("Unsupported codec {}", static_cast<int>(codec));
            return -1;
        }

        attr.stVencAttr.enType = codec;
        attr.stVencAttr.enPixelFormat = RK_FMT_RGB888;
        if (codec == RK_VIDEO_ID_AVC) {
            attr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
        }
        attr.stVencAttr.u32PicWidth = width;
        attr.stVencAttr.u32PicHeight = height;
        attr.stVencAttr.u32VirWidth = width;
        attr.stVencAttr.u32VirHeight = height;
        attr.stVencAttr.u32StreamBufCnt = 2;
        attr.stVencAttr.u32BufSize = width * height * 3 / 2;
        attr.stVencAttr.enMirror = MIRROR_NONE;

        auto ret = RK_MPI_VENC_CreateChn(chn_, &attr);
        if (ret != RK_SUCCESS) {
            SPDLOG_ERROR("RK_MPI_VENC_CreateChn failed {:x}", static_cast<unsigned int>(ret));
            return -1;
        }
        chn_created_ = true;

        VENC_RECV_PIC_PARAM_S recv_param;
        std::memset(&recv_param, 0, sizeof(recv_param));
        recv_param.s32RecvPicNum = -1;
        ret = RK_MPI_VENC_StartRecvFrame(chn_, &recv_param);
        if (ret != RK_SUCCESS) {
            SPDLOG_ERROR("RK_MPI_VENC_StartRecvFrame failed {:x}", static_cast<unsigned int>(ret));
            return -1;
        }
        recv_started_ = true;
        return 0;
    }

    int chn_{0};
    bool ok_{false};
    bool chn_created_{false};
    bool recv_started_{false};
};

} // namespace aipc::venc
