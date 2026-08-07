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

#include "aipc/native/aipf.h"

namespace media_worker {

constexpr std::size_t kAiFrameIpcHeaderSize = aipc::native::kAipfHeaderSize;
constexpr std::size_t kAiFrameIpcMaxPayload = aipc::native::kAipfMaxPayload;
using AiFitMode = aipc::native::AiFitMode;
using AiFrameTransform = aipc::native::AiFrameTransform;
using RawAiFrame = aipc::native::AiFrame;

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
