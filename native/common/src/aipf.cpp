#include "aipc/native/aipf.h"

#include <cstring>

#include "aipc/native/io.h"

namespace aipc::native {

std::vector<std::uint8_t> EncodeAipfFrame(const AiFrame& frame) {
    std::vector<std::uint8_t> output;
    output.reserve(kAipfHeaderSize + frame.data.size());
    output.insert(output.end(), {'A', 'I', 'P', 'F'});
    AppendU16(&output, kAipfVersion);
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

std::optional<AiFrame> ReadAipfFrame(int fd, std::string* error) {
    std::uint8_t header[kAipfHeaderSize];
    if (!ReadAll(fd, header, sizeof(header))) return std::nullopt;
    if (std::memcmp(header, "AIPF", 4) != 0 || ReadU16(header + 4) != kAipfVersion) {
        *error = "invalid AIPF header";
        return std::nullopt;
    }
    const auto fit_mode = ReadU16(header + 6);
    if (fit_mode > static_cast<std::uint16_t>(AiFitMode::kCover)) {
        *error = "invalid AIPF fit mode";
        return std::nullopt;
    }
    const std::size_t length = ReadU32(header + 8);
    AiFrame frame;
    frame.fit_mode = static_cast<AiFitMode>(fit_mode);
    frame.pts = ReadU64(header + 12);
    frame.sequence = ReadU64(header + 20);
    frame.width = ReadU32(header + 28);
    frame.height = ReadU32(header + 32);
    frame.y_stride = ReadU32(header + 36);
    frame.uv_stride = ReadU32(header + 40);
    frame.height_stride = ReadU32(header + 44);
    frame.main_width = ReadU32(header + 48);
    frame.main_height = ReadU32(header + 52);
    const std::size_t expected =
        static_cast<std::size_t>(frame.y_stride) * frame.height_stride * 3 / 2;
    if (length == 0 || length > kAipfMaxPayload || frame.width == 0 ||
        frame.height == 0 || frame.y_stride < frame.width ||
        frame.uv_stride < frame.width || frame.height_stride < frame.height ||
        length < expected) {
        *error = "inconsistent AIPF dimensions or payload length";
        return std::nullopt;
    }
    frame.data.resize(length);
    if (!ReadAll(fd, frame.data.data(), frame.data.size())) {
        *error = "truncated AIPF payload";
        return std::nullopt;
    }
    return frame;
}

std::vector<std::uint8_t> RepackNv12(const AiFrame& frame) {
    std::vector<std::uint8_t> packed(
        static_cast<std::size_t>(frame.width) * frame.height * 3 / 2);
    for (std::uint32_t row = 0; row < frame.height; ++row) {
        std::memcpy(packed.data() + static_cast<std::size_t>(row) * frame.width,
                    frame.data.data() + static_cast<std::size_t>(row) * frame.y_stride,
                    frame.width);
    }
    const std::size_t source_uv =
        static_cast<std::size_t>(frame.y_stride) * frame.height_stride;
    const std::size_t target_uv = static_cast<std::size_t>(frame.width) * frame.height;
    for (std::uint32_t row = 0; row < frame.height / 2; ++row) {
        std::memcpy(packed.data() + target_uv + static_cast<std::size_t>(row) * frame.width,
                    frame.data.data() + source_uv +
                        static_cast<std::size_t>(row) * frame.uv_stride,
                    frame.width);
    }
    return packed;
}

}  // namespace aipc::native
