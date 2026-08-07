#include "ai_frame_ipc_publisher.h"

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

void AppendI32(std::vector<std::uint8_t>* output, std::int32_t value) {
    AppendU32(output, static_cast<std::uint32_t>(value));
}

void AppendU64(std::vector<std::uint8_t>* output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output->push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

}  // namespace

std::vector<std::uint8_t> EncodeAiFrameIpcFrame(const RawAiFrame& frame) {
    std::vector<std::uint8_t> output;
    output.reserve(kAiFrameIpcHeaderSize + frame.data.size());
    output.insert(output.end(), {'A', 'I', 'P', 'F'});
    AppendU16(&output, kAiFrameIpcVersion);
    AppendU16(&output, static_cast<std::uint16_t>(frame.fit_mode));
    AppendU32(&output, static_cast<std::uint32_t>(frame.data.size()));
    AppendU64(&output, frame.pts);
    AppendU64(&output, frame.sequence);
    AppendU32(&output, frame.width);
    AppendU32(&output, frame.height);
    AppendU32(&output, frame.y_stride);
    AppendU32(&output, frame.uv_stride);
    AppendU32(&output, frame.height_stride);
    AppendU32(&output, frame.main_width);
    AppendU32(&output, frame.main_height);
    AppendI32(&output, frame.transform.crop_x);
    AppendI32(&output, frame.transform.crop_y);
    AppendI32(&output, frame.transform.crop_width);
    AppendI32(&output, frame.transform.crop_height);
    AppendI32(&output, frame.transform.pad_left);
    AppendI32(&output, frame.transform.pad_top);
    AppendI32(&output, frame.transform.pad_right);
    AppendI32(&output, frame.transform.pad_bottom);
    output.insert(output.end(), frame.data.begin(), frame.data.end());
    return output;
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
    std::size_t written = 0;
    while (running_.load() && written < message.size()) {
        const ssize_t result = write(fd_, message.data() + written, message.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        ReportError(std::string("AI frame IPC write failed: ") + std::strerror(errno));
        return false;
    }
    return written == message.size();
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
