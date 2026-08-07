#include "media_control.h"

#include <cerrno>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace media_worker {

MediaControl::MediaControl(int fd, Handler handler)
    : fd_(fd), handler_(std::move(handler)) {}

MediaControl::~MediaControl() {
    Stop();
}

bool MediaControl::Start() {
    if (fd_ < 0 || running_.exchange(true)) return false;
    try {
        thread_ = std::thread(&MediaControl::Run, this);
    } catch (...) {
        running_.store(false);
        return false;
    }
    return true;
}

void MediaControl::Stop() {
    if (!running_.exchange(false) && fd_ < 0) return;
    if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void MediaControl::Run() {
    constexpr std::uint32_t kMaxControlPayload = 256 * 1024;
    while (running_.load()) {
        std::uint8_t length_bytes[4];
        if (!ReadAll(length_bytes, sizeof(length_bytes))) break;
        const std::uint32_t length = (static_cast<std::uint32_t>(length_bytes[0]) << 24) |
                                     (static_cast<std::uint32_t>(length_bytes[1]) << 16) |
                                     (static_cast<std::uint32_t>(length_bytes[2]) << 8) |
                                     static_cast<std::uint32_t>(length_bytes[3]);
        if (length == 0 || length > kMaxControlPayload) break;
        std::vector<std::uint8_t> payload(length);
        if (!ReadAll(payload.data(), payload.size())) break;
        nlohmann::json response;
        try {
            const auto request = nlohmann::json::parse(payload.begin(), payload.end());
            response = handler_(request);
        } catch (const std::exception& exception) {
            response = {{"version", 1}, {"type", "error"}, {"error", exception.what()}};
        }
        if (!WriteMessage(response)) break;
    }
    running_.store(false);
}

bool MediaControl::ReadAll(void* data, std::size_t size) {
    auto* output = static_cast<std::uint8_t*>(data);
    std::size_t offset = 0;
    while (running_.load() && offset < size) {
        const ssize_t result = read(fd_, output + offset, size - offset);
        if (result > 0) {
            offset += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        return false;
    }
    return offset == size;
}

bool MediaControl::WriteAll(const void* data, std::size_t size) {
    const auto* input = static_cast<const std::uint8_t*>(data);
    std::size_t offset = 0;
    while (running_.load() && offset < size) {
        const ssize_t result = write(fd_, input + offset, size - offset);
        if (result > 0) {
            offset += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        return false;
    }
    return offset == size;
}

bool MediaControl::WriteMessage(const nlohmann::json& message) {
    const std::string payload = message.dump();
    const std::uint32_t length = static_cast<std::uint32_t>(payload.size());
    const std::uint8_t header[4] = {
        static_cast<std::uint8_t>(length >> 24),
        static_cast<std::uint8_t>(length >> 16),
        static_cast<std::uint8_t>(length >> 8),
        static_cast<std::uint8_t>(length),
    };
    return WriteAll(header, sizeof(header)) && WriteAll(payload.data(), payload.size());
}

}  // namespace media_worker
