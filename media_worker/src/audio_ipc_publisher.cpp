#include "audio_ipc_publisher.h"

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

std::vector<std::uint8_t> EncodeAudioIpcFrame(const EncodedAudioFrame& frame) {
    std::vector<std::uint8_t> message;
    message.reserve(kAudioIpcHeaderSize + frame.data.size());
    message.insert(message.end(), {'A', 'I', 'P', 'A'});
    AppendU16(&message, kAudioIpcVersion);
    AppendU16(&message, 0);
    AppendU32(&message, static_cast<std::uint32_t>(frame.data.size()));
    AppendU64(&message, frame.pts);
    AppendU64(&message, frame.sequence);
    message.insert(message.end(), frame.data.begin(), frame.data.end());
    return message;
}

AudioIpcPublisher::AudioIpcPublisher(int fd, std::size_t capacity,
                                     ErrorCallback error_callback)
    : fd_(fd), capacity_(capacity == 0 ? 1 : capacity),
      error_callback_(std::move(error_callback)) {}

AudioIpcPublisher::~AudioIpcPublisher() {
    Stop();
}

bool AudioIpcPublisher::Start() {
    if (fd_ < 0 || running_.exchange(true)) return false;
    std::signal(SIGPIPE, SIG_IGN);
    try {
        writer_thread_ = std::thread(&AudioIpcPublisher::WriteLoop, this);
    } catch (const std::exception& exception) {
        running_.store(false);
        ReportError(std::string("cannot start audio IPC writer: ") + exception.what());
        return false;
    }
    return true;
}

bool AudioIpcPublisher::Enqueue(EncodedAudioFrame frame) {
    if (!running_.load()) return false;
    if (frame.data.empty() || frame.data.size() > kAudioIpcMaxPayload) {
        drops_.fetch_add(1);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return false;
        if (frames_queue_.size() >= capacity_) {
            frames_queue_.pop_front();
            drops_.fetch_add(1);
        }
        frames_queue_.push_back(std::move(frame));
    }
    condition_.notify_one();
    return true;
}

void AudioIpcPublisher::Stop() {
    running_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        frames_queue_.clear();
    }
    condition_.notify_all();
    if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
    if (writer_thread_.joinable()) writer_thread_.join();
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void AudioIpcPublisher::WriteLoop() {
    while (running_.load()) {
        EncodedAudioFrame frame;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] {
                return stopped_ || !running_.load() || !frames_queue_.empty();
            });
            if (frames_queue_.empty()) break;
            frame = std::move(frames_queue_.front());
            frames_queue_.pop_front();
        }
        const std::size_t payload_size = frame.data.size();
        if (!WriteAll(EncodeAudioIpcFrame(frame))) break;
        frames_.fetch_add(1);
        bytes_.fetch_add(payload_size);
    }
    running_.store(false);
}

bool AudioIpcPublisher::WriteAll(const std::vector<std::uint8_t>& message) {
    std::size_t written = 0;
    while (running_.load() && written < message.size()) {
        const ssize_t result = write(fd_, message.data() + written, message.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        ReportError(std::string("audio IPC write failed: ") + std::strerror(errno));
        return false;
    }
    return written == message.size();
}

void AudioIpcPublisher::ReportError(const std::string& message) {
    errors_.fetch_add(1);
    if (!error_reported_.exchange(true) && error_callback_) error_callback_(message);
}

}  // namespace media_worker
