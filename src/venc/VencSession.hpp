#pragma once

#include <spdlog/spdlog.h>

#include "venc/venc.h"

namespace aipc::venc {

class VencSession {
public:
    VencSession(int chn, int width, int height, RK_CODEC_ID_E codec) : chn_(chn) {
        const int ret = venc_init(chn_, width, height, codec);
        if (ret != 0) {
            SPDLOG_ERROR("venc_init failed ret={}", ret);
            ok_ = false;
            return;
        }
        ok_ = true;
    }

    ~VencSession() {
        if (!ok_) {
            return;
        }

        const auto ret1 = RK_MPI_VENC_StopRecvFrame(chn_);
        if (ret1 != RK_SUCCESS) {
            SPDLOG_WARN("RK_MPI_VENC_StopRecvFrame failed {:x}", static_cast<unsigned int>(ret1));
        }

        const auto ret2 = RK_MPI_VENC_DestroyChn(chn_);
        if (ret2 != RK_SUCCESS) {
            SPDLOG_WARN("RK_MPI_VENC_DestroyChn failed {:x}", static_cast<unsigned int>(ret2));
        }
    }

    VencSession(const VencSession &) = delete;
    VencSession &operator=(const VencSession &) = delete;
    VencSession(VencSession &&) = delete;
    VencSession &operator=(VencSession &&) = delete;

    bool ok() const { return ok_; }

private:
    int chn_{0};
    bool ok_{false};
};

} // namespace aipc::venc
