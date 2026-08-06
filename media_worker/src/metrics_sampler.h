#pragma once

#include <cstdint>

namespace media_worker {

struct RateSample {
    double fps = 0.0;
    double bitrate_kbps = 0.0;
};

class MetricsSampler {
public:
    RateSample Sample(std::uint64_t packets, std::uint64_t bytes, double elapsed_seconds);

private:
    bool initialized_ = false;
    std::uint64_t previous_packets_ = 0;
    std::uint64_t previous_bytes_ = 0;
    double previous_elapsed_seconds_ = 0.0;
};

}  // namespace media_worker
