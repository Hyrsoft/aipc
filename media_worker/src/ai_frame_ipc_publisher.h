#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace media_worker {

constexpr std::size_t kAiFrameIpcHeaderSize = 88;
constexpr std::uint16_t kAiFrameIpcVersion = 1;
constexpr std::size_t kAiFrameIpcMaxPayload = 8 * 1024 * 1024;

enum class AiFitMode : std::uint16_t {
    kStretch = 0,
    kContain = 1,
    kCover = 2,
};

struct AiFrameTransform {
    std::int32_t crop_x = 0;
    std::int32_t crop_y = 0;
    std::int32_t crop_width = 0;
    std::int32_t crop_height = 0;
    std::int32_t pad_left = 0;
    std::int32_t pad_top = 0;
    std::int32_t pad_right = 0;
    std::int32_t pad_bottom = 0;
};

struct RawAiFrame {
    std::vector<std::uint8_t> data;
    std::uint64_t pts = 0;
    std::uint64_t sequence = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t y_stride = 0;
    std::uint32_t uv_stride = 0;
    std::uint32_t height_stride = 0;
    std::uint32_t main_width = 0;
    std::uint32_t main_height = 0;
    AiFitMode fit_mode = AiFitMode::kStretch;
    AiFrameTransform transform;
};

std::vector<std::uint8_t> EncodeAiFrameIpcFrame(const RawAiFrame& frame);

class AiFrameIpcPublisher {
public:
    using ErrorCallback = std::function<void(const std::string&)>;

    explicit AiFrameIpcPublisher(int fd, ErrorCallback error_callback = {});
    ~AiFrameIpcPublisher();

    bool Start();
    bool Enqueue(RawAiFrame frame);
    void Stop();

    std::uint64_t Frames() const { return frames_.load(); }
    std::uint64_t Bytes() const { return bytes_.load(); }
    std::uint64_t Drops() const { return drops_.load(); }
    std::uint64_t Errors() const { return errors_.load(); }

private:
    void WriteLoop();
    bool WriteAll(const std::vector<std::uint8_t>& message);
    void ReportError(const std::string& message);

    int fd_;
    ErrorCallback error_callback_;
    std::atomic<bool> running_{false};
    std::atomic<bool> error_reported_{false};
    std::thread writer_thread_;
    std::mutex mutex_;
    std::condition_variable condition_;
    RawAiFrame pending_;
    bool has_pending_ = false;
    bool stopped_ = false;
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<std::uint64_t> drops_{0};
    std::atomic<std::uint64_t> errors_{0};
};

AiFitMode ParseAiFitMode(const std::string& value);

}  // namespace media_worker
