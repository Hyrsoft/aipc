#include "metrics_sampler.h"

namespace media_worker {

RateSample MetricsSampler::Sample(std::uint64_t packets, std::uint64_t bytes,
                                  double elapsed_seconds) {
    const double interval_seconds =
        initialized_ ? elapsed_seconds - previous_elapsed_seconds_ : elapsed_seconds;
    if (interval_seconds <= 0.0) return {};

    const std::uint64_t packet_delta =
        initialized_ && packets >= previous_packets_ ? packets - previous_packets_ : packets;
    const std::uint64_t byte_delta =
        initialized_ && bytes >= previous_bytes_ ? bytes - previous_bytes_ : bytes;

    initialized_ = true;
    previous_packets_ = packets;
    previous_bytes_ = bytes;
    previous_elapsed_seconds_ = elapsed_seconds;

    return {
        static_cast<double>(packet_delta) / interval_seconds,
        static_cast<double>(byte_delta) * 8.0 / interval_seconds / 1000.0,
    };
}

}  // namespace media_worker
