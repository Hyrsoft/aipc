#pragma once

#include <string>

#include "config.h"
#include "event_emitter.h"

namespace media_worker {

class MediaRuntime {
public:
    MediaRuntime(const IspConfig& config, EventEmitter* events);
    ~MediaRuntime();

    MediaRuntime(const MediaRuntime&) = delete;
    MediaRuntime& operator=(const MediaRuntime&) = delete;

    bool Init(std::string* error);
    void Deinit();

private:
    IspConfig config_;
    EventEmitter* events_;
    bool isp_initialized_ = false;
    bool isp_running_ = false;
    bool mpi_initialized_ = false;
};

}  // namespace media_worker
