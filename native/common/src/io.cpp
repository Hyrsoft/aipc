#include "aipc/native/io.h"

#include <cerrno>
#include <string>

#include <unistd.h>

namespace aipc::native {

bool ReadAll(int fd, void* output, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(output);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t result = read(fd, bytes + offset, size - offset);
        if (result > 0) {
            offset += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool WriteAll(int fd, const void* input, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(input);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t result = write(fd, bytes + offset, size - offset);
        if (result > 0) {
            offset += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool WriteJsonMessage(int fd, const nlohmann::json& value, std::size_t max_bytes) {
    const std::string payload = value.dump();
    if (payload.empty() || payload.size() > max_bytes) return false;
    std::vector<std::uint8_t> header;
    header.reserve(4);
    AppendU32(&header, static_cast<std::uint32_t>(payload.size()));
    return WriteAll(fd, header.data(), header.size()) &&
           WriteAll(fd, payload.data(), payload.size());
}

std::uint16_t ReadU16(const std::uint8_t* data) {
    return (static_cast<std::uint16_t>(data[0]) << 8) |
           static_cast<std::uint16_t>(data[1]);
}

std::uint32_t ReadU32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

std::uint64_t ReadU64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) value = (value << 8) | data[index];
    return value;
}

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

}  // namespace aipc::native
