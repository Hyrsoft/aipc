#include "aipc/native/aipv2.h"

#include <cstring>

#include "aipc/native/io.h"

namespace aipc::native {

std::vector<std::uint8_t> EncodeAipv2AccessUnit(const EncodedAccessUnit& frame) {
    std::vector<std::uint8_t> output;
    output.reserve(kAipv2HeaderSize + frame.data.size());
    output.insert(output.end(), {'A', 'I', 'P', 'V'});
    AppendU16(&output, kAipv2Version);
    AppendU16(&output, frame.flags);
    AppendU32(&output, static_cast<std::uint32_t>(frame.data.size()));
    AppendU64(&output, frame.pts);
    AppendU64(&output, frame.sequence);
    AppendU32(&output, 0);
    output.insert(output.end(), frame.data.begin(), frame.data.end());
    return output;
}

std::optional<EncodedAccessUnit> ReadAipv2AccessUnit(int fd, std::string* error) {
    std::uint8_t header[kAipv2HeaderSize];
    if (!ReadAll(fd, header, sizeof(header))) return std::nullopt;
    if (std::memcmp(header, "AIPV", 4) != 0 || ReadU16(header + 4) != kAipv2Version) {
        *error = "invalid AIPV2 header";
        return std::nullopt;
    }
    EncodedAccessUnit frame;
    frame.flags = ReadU16(header + 6);
    if ((frame.flags & ~(kAipv2Keyframe | kAipv2Discontinuity |
                         kAipv2CodecConfig | kAipv2EndOfStream)) != 0) {
        *error = "invalid AIPV2 flags";
        return std::nullopt;
    }
    const std::size_t length = ReadU32(header + 8);
    if (length > kAipv2MaxPayload || (length == 0 && !frame.end_of_stream())) {
        *error = "invalid AIPV2 payload length";
        return std::nullopt;
    }
    frame.pts = ReadU64(header + 12);
    frame.sequence = ReadU64(header + 20);
    frame.data.resize(length);
    if (length > 0 && !ReadAll(fd, frame.data.data(), length)) {
        *error = "truncated AIPV2 payload";
        return std::nullopt;
    }
    return frame;
}

}  // namespace aipc::native
