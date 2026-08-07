#include "aipc/native/validation.h"

namespace aipc::native {

bool IsSafeFileName(const std::string& value, std::size_t max_length) {
    if (value.empty() || value.size() > max_length || value == "." || value == "..") {
        return false;
    }
    for (const unsigned char byte : value) {
        if (!(byte >= '0' && byte <= '9') && !(byte >= 'A' && byte <= 'Z') &&
            !(byte >= 'a' && byte <= 'z') && byte != '.' && byte != '_' && byte != '-') {
            return false;
        }
    }
    return true;
}

}  // namespace aipc::native
