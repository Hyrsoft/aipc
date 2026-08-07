#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aipc::native {

constexpr std::size_t kAipfHeaderSize = 88;
constexpr std::uint16_t kAipfVersion = 1;
constexpr std::size_t kAipfMaxPayload = 8 * 1024 * 1024;

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

struct AiFrame {
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

std::vector<std::uint8_t> EncodeAipfFrame(const AiFrame& frame);
std::optional<AiFrame> ReadAipfFrame(int fd, std::string* error);
std::vector<std::uint8_t> RepackNv12(const AiFrame& frame);

}  // namespace aipc::native
