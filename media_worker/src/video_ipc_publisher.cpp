#include "video_ipc_publisher.h"

#include <cerrno>
#include <csignal>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

namespace media_worker {
namespace {

void AppendU16(std::vector<std::uint8_t>* output, std::uint16_t value) {
    output->push_back(static_cast<std::uint8_t>(value >> 8));
    output->push_back(static_cast<std::uint8_t>(value));
}

void AppendU32(std::vector<std::uint8_t>* output, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output->push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void AppendU64(std::vector<std::uint8_t>* output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output->push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

}  // namespace

std::vector<std::uint8_t> EncodeVideoIpcFrame(const EncodedVideoFrame& frame) {
    std::vector<std::uint8_t> message;
    message.reserve(kVideoIpcHeaderSize + frame.data.size());
    message.insert(message.end(), {'A', 'I', 'P', 'V'});
    AppendU16(&message, kVideoIpcVersion);
    AppendU16(&message, frame.keyframe ? kVideoIpcKeyframe : 0);
    AppendU32(&message, static_cast<std::uint32_t>(frame.data.size()));
    AppendU64(&message, frame.pts);
    AppendU64(&message, frame.sequence);
    message.insert(message.end(), frame.data.begin(), frame.data.end());
    return message;
}

VideoFrameQueue::VideoFrameQueue(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

VideoFrameQueue::PushResult VideoFrameQueue::Push(EncodedVideoFrame frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    PushResult result;
    if (stopped_) return result;
    if (awaiting_keyframe_ && !frame.keyframe) {
        result.dropped = 1;
        return result;
    }
    if (frame.keyframe) {
        awaiting_keyframe_ = false;
        result.dropped = frames_.size();
        frames_.clear();
    } else if (frames_.size() >= capacity_) {
        result.dropped = frames_.size() + 1;
        frames_.clear();
        awaiting_keyframe_ = true;
        result.request_idr = true;
        return result;
    }
    frames_.push_back(std::move(frame));
    result.accepted = true;
    condition_.notify_one();
    return result;
}

bool VideoFrameQueue::WaitPop(EncodedVideoFrame* frame,
                              const std::atomic<bool>& running) {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [&] { return stopped_ || !running.load() || !frames_.empty(); });
    if (frames_.empty()) return false;
    *frame = std::move(frames_.front());
    frames_.pop_front();
    return true;
}

void VideoFrameQueue::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    frames_.clear();
    condition_.notify_all();
}

VideoIpcPublisher::VideoIpcPublisher(int fd, std::size_t capacity,
                                     ErrorCallback error_callback)
    : fd_(fd), queue_(capacity), error_callback_(std::move(error_callback)) {}

VideoIpcPublisher::~VideoIpcPublisher() {
    Stop();
}

bool VideoIpcPublisher::Start() {
    if (fd_ < 0 || running_.exchange(true)) return false;
    std::signal(SIGPIPE, SIG_IGN);
    try {
        writer_thread_ = std::thread(&VideoIpcPublisher::WriteLoop, this);
    } catch (const std::exception& exception) {
        running_.store(false);
        ReportError(std::string("cannot start video IPC writer: ") + exception.what());
        return false;
    }
    return true;
}

bool VideoIpcPublisher::Enqueue(EncodedVideoFrame frame) {
    if (!running_.load()) return false;
    if (frame.data.size() > kVideoIpcMaxPayload) {
        drops_.fetch_add(1);
        ReportError("video IPC frame exceeds 4 MiB limit");
        return true;
    }
    const auto result = queue_.Push(std::move(frame));
    drops_.fetch_add(result.dropped);
    return result.request_idr;
}

void VideoIpcPublisher::Stop() {
    running_.store(false);
    queue_.Stop();
    if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
    if (writer_thread_.joinable()) writer_thread_.join();
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void VideoIpcPublisher::WriteLoop() {
    EncodedVideoFrame frame;
    while (running_.load() && queue_.WaitPop(&frame, running_)) {
        const std::size_t payload_size = frame.data.size();
        const auto message = EncodeVideoIpcFrame(frame);
        if (!WriteAll(message)) break;
        frames_.fetch_add(1);
        bytes_.fetch_add(payload_size);
    }
    running_.store(false);
}

bool VideoIpcPublisher::WriteAll(const std::vector<std::uint8_t>& message) {
    std::size_t written = 0;
    while (running_.load() && written < message.size()) {
        const ssize_t result = write(fd_, message.data() + written, message.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        ReportError(std::string("video IPC write failed: ") + std::strerror(errno));
        return false;
    }
    return written == message.size();
}

void VideoIpcPublisher::ReportError(const std::string& message) {
    errors_.fetch_add(1);
    if (!error_reported_.exchange(true) && error_callback_) error_callback_(message);
}

}  // namespace media_worker
