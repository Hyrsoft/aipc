#pragma once

#include <nlohmann/json.hpp>

#include "config.h"

namespace media_worker {

class VideoPipeline;

class MediaControlService {
public:
    MediaControlService(WorkerConfig* config, VideoPipeline* video);

    nlohmann::json Handle(const nlohmann::json& request);

private:
    WorkerConfig* config_;
    VideoPipeline* video_;
};

}  // namespace media_worker
