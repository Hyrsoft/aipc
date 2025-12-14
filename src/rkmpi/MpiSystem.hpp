#pragma once

#include <spdlog/spdlog.h>

#include "sample_comm.h"

namespace aipc::rkmpi {

    class MpiSystem {
    public:
        MpiSystem() {
            const auto ret = RK_MPI_SYS_Init();
            if (ret != RK_SUCCESS) {
                SPDLOG_ERROR("RK_MPI_SYS_Init failed: {:x}", static_cast<unsigned int>(ret));
                ok_ = false;
            }
        }

        ~MpiSystem() {
            if (ok_) {
                RK_MPI_SYS_Exit();
            }
        }

        MpiSystem(const MpiSystem &) = delete;
        MpiSystem &operator=(const MpiSystem &) = delete;

        MpiSystem(MpiSystem &&) = delete;
        MpiSystem &operator=(MpiSystem &&) = delete;

        bool ok() const { return ok_; }

    private:
        bool ok_{true};
    };

} // namespace aipc::rkmpi
