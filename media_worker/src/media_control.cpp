#include "media_control.h"

#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include "aipc/native/io.h"

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
        if (!aipc::native::ReadAll(fd_, length_bytes, sizeof(length_bytes))) break;
        const std::uint32_t length = aipc::native::ReadU32(length_bytes);
        if (length == 0 || length > kMaxControlPayload) break;
        std::vector<std::uint8_t> payload(length);
        if (!aipc::native::ReadAll(fd_, payload.data(), payload.size())) break;
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

bool MediaControl::WriteMessage(const nlohmann::json& message) {
    return aipc::native::WriteJsonMessage(fd_, message);
}

}  // namespace media_worker
