#pragma once

#include <spdlog/spdlog.h>

#include "sample_comm.h"

namespace aipc::rkmpi {

    inline bool ok(RK_S32 ret, const char *expr, const char *file, int line) {
        if (ret == RK_SUCCESS) {
            return true;
        }
        SPDLOG_ERROR("MPI call failed: {} ret={:x} at {}:{}", expr, static_cast<unsigned int>(ret), file, line);
        return false;
    }

} // namespace aipc::rkmpi

#define AIPC_CHECK_MPI(expr) (::aipc::rkmpi::ok((expr), #expr, __FILE__, __LINE__))
