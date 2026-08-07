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

std::string ErrorMessage(const std::string& component, const std::string& operation,
                         const std::string& detail) {
    return component + ": " + operation + ": " + detail;
}

}  // namespace aipc::native
