#pragma once

#include <cstddef>
#include <string>

namespace aipc::native {

bool IsSafeFileName(const std::string& value, std::size_t max_length = 128);
std::string ErrorMessage(const std::string& component, const std::string& operation,
                         const std::string& detail);

}  // namespace aipc::native
