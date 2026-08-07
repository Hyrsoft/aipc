#pragma once

#include <cstddef>
#include <string>

namespace aipc::native {

bool IsSafeFileName(const std::string& value, std::size_t max_length = 128);

}  // namespace aipc::native
