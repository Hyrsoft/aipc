#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace media_worker {

constexpr std::size_t kAudioIpcHeaderSize = 28;
constexpr std::uint16_t kAudioIpcVersion = 1;
constexpr std::size_t kAudioIpcMaxPayload = 64 * 1024;

struct EncodedAudioFrame {
    std::vector<std::uint8_t> data;
    std::uint64_t pts = 0;
    std::uint64_t sequence = 0;
};

std::vector<std::uint8_t> EncodeAudioIpcFrame(const EncodedAudioFrame& frame);

class AudioIpcPublisher {
public:
    using ErrorCallback = std::function<void(const std::string&)>;

    explicit AudioIpcPublisher(int fd, std::size_t capacity = 16,
                               ErrorCallback error_callback = {});
    ~AudioIpcPublisher();

    AudioIpcPublisher(const AudioIpcPublisher&) = delete;
    AudioIpcPublisher& operator=(const AudioIpcPublisher&) = delete;

    bool Start();
    bool Enqueue(EncodedAudioFrame frame);
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
    const std::size_t capacity_;
    ErrorCallback error_callback_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<EncodedAudioFrame> frames_queue_;
    bool stopped_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> error_reported_{false};
    std::thread writer_thread_;
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<std::uint64_t> drops_{0};
    std::atomic<std::uint64_t> errors_{0};
};

}  // namespace media_worker
