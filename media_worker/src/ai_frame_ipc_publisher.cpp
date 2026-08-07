#include "ai_frame_ipc_publisher.h"

#include <cerrno>
#include <csignal>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include "aipc/native/io.h"

namespace media_worker {
std::vector<std::uint8_t> EncodeAiFrameIpcFrame(const RawAiFrame& frame) {
    return aipc::native::EncodeAipfFrame(frame);
}

AiFrameIpcPublisher::AiFrameIpcPublisher(int fd, ErrorCallback error_callback)
    : fd_(fd), error_callback_(std::move(error_callback)) {}

AiFrameIpcPublisher::~AiFrameIpcPublisher() {
    Stop();
}

bool AiFrameIpcPublisher::Start() {
    if (fd_ < 0 || running_.exchange(true)) return false;
    std::signal(SIGPIPE, SIG_IGN);
    try {
        writer_thread_ = std::thread(&AiFrameIpcPublisher::WriteLoop, this);
    } catch (const std::exception& exception) {
        running_.store(false);
        ReportError(std::string("cannot start AI frame IPC writer: ") + exception.what());
        return false;
    }
    return true;
}

bool AiFrameIpcPublisher::Enqueue(RawAiFrame frame) {
    if (!running_.load()) return false;
    if (frame.data.empty() || frame.data.size() > kAiFrameIpcMaxPayload) {
        drops_.fetch_add(1);
        ReportError("AI IPC frame is empty or exceeds 8 MiB limit");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return false;
        if (has_pending_) drops_.fetch_add(1);
        pending_ = std::move(frame);
        has_pending_ = true;
    }
    condition_.notify_one();
    return true;
}

void AiFrameIpcPublisher::Stop() {
    if (!running_.exchange(false) && fd_ < 0) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        has_pending_ = false;
    }
    condition_.notify_all();
    if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
    if (writer_thread_.joinable()) writer_thread_.join();
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void AiFrameIpcPublisher::WriteLoop() {
    while (running_.load()) {
        RawAiFrame frame;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopped_ || has_pending_; });
            if (stopped_ || !running_.load()) break;
            frame = std::move(pending_);
            has_pending_ = false;
        }
        const std::size_t payload_size = frame.data.size();
        if (!WriteAll(EncodeAiFrameIpcFrame(frame))) break;
        frames_.fetch_add(1);
        bytes_.fetch_add(payload_size);
    }
    running_.store(false);
}

bool AiFrameIpcPublisher::WriteAll(const std::vector<std::uint8_t>& message) {
    if (!running_.load() ||
        !aipc::native::WriteAll(fd_, message.data(), message.size())) {
        ReportError(std::string("AI frame IPC write failed: ") + std::strerror(errno));
        return false;
    }
    return true;
}

void AiFrameIpcPublisher::ReportError(const std::string& message) {
    errors_.fetch_add(1);
    if (!error_reported_.exchange(true) && error_callback_) error_callback_(message);
}

AiFitMode ParseAiFitMode(const std::string& value) {
    if (value == "contain") return AiFitMode::kContain;
    if (value == "cover") return AiFitMode::kCover;
    return AiFitMode::kStretch;
}

}  // namespace media_worker
