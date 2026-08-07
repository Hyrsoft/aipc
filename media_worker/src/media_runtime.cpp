#include "media_runtime.h"

#include "rk_mpi_sys.h"
#include "sample_comm.h"

namespace media_worker {

MediaRuntime::MediaRuntime(const IspConfig& config, EventEmitter* events)
    : config_(config), events_(events) {}

MediaRuntime::~MediaRuntime() {
    Deinit();
}

bool MediaRuntime::Init(std::string* error) {
    events_->Emit("BootProgress", {{"stage", "isp_initializing"}});
    RK_S32 result = SAMPLE_COMM_ISP_Init(config_.camera_id, RK_AIQ_WORKING_MODE_NORMAL,
                                         RK_FALSE, config_.iq_dir.c_str());
    if (result != RK_SUCCESS) {
        *error = "SAMPLE_COMM_ISP_Init failed: " + std::to_string(result);
        return false;
    }
    isp_initialized_ = true;
    result = SAMPLE_COMM_ISP_Run(config_.camera_id);
    if (result != RK_SUCCESS) {
        *error = "SAMPLE_COMM_ISP_Run failed: " + std::to_string(result);
        return false;
    }
    isp_running_ = true;
    events_->Emit("BootProgress", {{"stage", "isp_ready"}});

    result = RK_MPI_SYS_Init();
    if (result != RK_SUCCESS) {
        *error = "RK_MPI_SYS_Init failed: " + std::to_string(result);
        return false;
    }
    mpi_initialized_ = true;
    events_->Emit("BootProgress", {{"stage", "mpi_ready"}});
    return true;
}

void MediaRuntime::Deinit() {
    if (mpi_initialized_) {
        RK_MPI_SYS_Exit();
        mpi_initialized_ = false;
    }
    if (isp_running_ || isp_initialized_) {
        SAMPLE_COMM_ISP_Stop(config_.camera_id);
        isp_running_ = false;
        isp_initialized_ = false;
    }
}

}  // namespace media_worker
