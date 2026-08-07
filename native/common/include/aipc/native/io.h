#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <nlohmann/json.hpp>

namespace aipc::native {

constexpr std::size_t kMaxJsonMessageBytes = 256 * 1024;

bool ReadAll(int fd, void* output, std::size_t size);
bool WriteAll(int fd, const void* input, std::size_t size);
bool WriteJsonMessage(int fd, const nlohmann::json& value,
                      std::size_t max_bytes = kMaxJsonMessageBytes);

std::uint16_t ReadU16(const std::uint8_t* data);
std::uint32_t ReadU32(const std::uint8_t* data);
std::uint64_t ReadU64(const std::uint8_t* data);
void AppendU16(std::vector<std::uint8_t>* output, std::uint16_t value);
void AppendU32(std::vector<std::uint8_t>* output, std::uint32_t value);
void AppendI32(std::vector<std::uint8_t>* output, std::int32_t value);
void AppendU64(std::vector<std::uint8_t>* output, std::uint64_t value);

}  // namespace aipc::native
