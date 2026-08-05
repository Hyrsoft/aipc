#include "event_emitter.h"

#include <cerrno>
#include <cstdint>
#include <unistd.h>

#include <iostream>
#include <utility>

namespace media_worker {

EventEmitter::EventEmitter(std::string generation)
    : generation_(std::move(generation)),
      start_time_(std::chrono::steady_clock::now()),
      output_fd_(dup(STDOUT_FILENO)) {}

EventEmitter::~EventEmitter() {
    if (output_fd_ >= 0) close(output_fd_);
}

void EventEmitter::Emit(const std::string& event, nlohmann::json fields) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time_);
    nlohmann::json message = {
        {"schema_version", 1},
        {"event", event},
        {"generation", generation_},
        {"monotonic_ms", elapsed.count()},
    };
    if (fields.is_object()) {
        message.update(fields);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string line = message.dump() + "\n";
    if (output_fd_ < 0) {
        std::cerr << line;
        return;
    }
    std::size_t written = 0;
    while (written < line.size()) {
        const ssize_t result = write(output_fd_, line.data() + written, line.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

}  // namespace media_worker
