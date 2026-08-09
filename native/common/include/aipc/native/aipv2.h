#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aipc::native {

constexpr std::size_t kAipv2HeaderSize = 32;
constexpr std::uint16_t kAipv2Version = 2;
constexpr std::size_t kAipv2MaxPayload = 16 * 1024 * 1024;

enum Aipv2Flag : std::uint16_t {
    kAipv2Keyframe = 1 << 0,
    kAipv2Discontinuity = 1 << 1,
    kAipv2CodecConfig = 1 << 2,
    kAipv2EndOfStream = 1 << 3,
};

struct EncodedAccessUnit {
    std::vector<std::uint8_t> data;
    std::uint64_t pts = 0;
    std::uint64_t sequence = 0;
    std::uint16_t flags = 0;

    bool keyframe() const { return (flags & kAipv2Keyframe) != 0; }
    bool discontinuity() const { return (flags & kAipv2Discontinuity) != 0; }
    bool end_of_stream() const { return (flags & kAipv2EndOfStream) != 0; }
};

std::vector<std::uint8_t> EncodeAipv2AccessUnit(const EncodedAccessUnit& frame);
std::optional<EncodedAccessUnit> ReadAipv2AccessUnit(int fd, std::string* error);

}  // namespace aipc::native
