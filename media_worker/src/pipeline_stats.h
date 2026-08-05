#pragma once

#include <atomic>
#include <cstdint>

#include <nlohmann/json.hpp>

namespace media_worker {

struct PipelineStats {
    std::atomic<std::uint64_t> packets{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> keyframes{0};
    std::atomic<std::uint64_t> timeouts{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::uint64_t> last_pts{0};

    nlohmann::json Snapshot() const {
        return {
            {"packets", packets.load()},
            {"bytes", bytes.load()},
            {"keyframes", keyframes.load()},
            {"timeouts", timeouts.load()},
            {"errors", errors.load()},
            {"last_pts", last_pts.load()},
        };
    }
};

}  // namespace media_worker

