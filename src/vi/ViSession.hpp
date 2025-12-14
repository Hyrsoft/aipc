#pragma once

#include <spdlog/spdlog.h>

#include "vi/vi.h"

namespace aipc::vi {

class ViSession {
public:
    ViSession(int dev, int chn, int width, int height) : dev_(dev), chn_(chn) {
        if (vi_dev_init() != 0) {
            SPDLOG_ERROR("vi_dev_init failed");
            ok_ = false;
            return;
        }
        const int ret = vi_chn_init(chn_, width, height);
        if (ret != 0) {
            SPDLOG_ERROR("vi_chn_init failed ret={}", ret);
            ok_ = false;
            return;
        }
        ok_ = true;
    }

    ~ViSession() {
        if (!ok_) {
            return;
        }

        const auto ret1 = RK_MPI_VI_DisableChn(dev_, chn_);
        if (ret1 != RK_SUCCESS) {
            SPDLOG_WARN("RK_MPI_VI_DisableChn failed {:x}", static_cast<unsigned int>(ret1));
        }

        const auto ret2 = RK_MPI_VI_DisableDev(dev_);
        if (ret2 != RK_SUCCESS) {
            SPDLOG_WARN("RK_MPI_VI_DisableDev failed {:x}", static_cast<unsigned int>(ret2));
        }
    }

    ViSession(const ViSession &) = delete;
    ViSession &operator=(const ViSession &) = delete;
    ViSession(ViSession &&) = delete;
    ViSession &operator=(ViSession &&) = delete;

    bool ok() const { return ok_; }

private:
    int dev_{0};
    int chn_{0};
    bool ok_{false};
};

} // namespace aipc::vi
