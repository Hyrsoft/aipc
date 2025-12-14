#pragma once

#include <spdlog/spdlog.h>

#include "sample_comm.h"

namespace aipc::rkmpi {

    class IspSession {
    public:
        IspSession(int cam_id, rk_aiq_working_mode_t mode, RK_BOOL multi_sensor, const char *iq_dir) : cam_id_(cam_id) {
            SAMPLE_COMM_ISP_Init(cam_id_, mode, multi_sensor, iq_dir);
            SAMPLE_COMM_ISP_Run(cam_id_);
            ok_ = true;
        }

        ~IspSession() {
            if (ok_) {
                SAMPLE_COMM_ISP_Stop(cam_id_);
            }
        }

        IspSession(const IspSession &) = delete;
        IspSession &operator=(const IspSession &) = delete;

        IspSession(IspSession &&) = delete;
        IspSession &operator=(IspSession &&) = delete;

        bool ok() const { return ok_; }

    private:
        int cam_id_{0};
        bool ok_{false};
    };

} // namespace aipc::rkmpi
