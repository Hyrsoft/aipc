#pragma once

#include <memory>
#include <string>

#include "types.h"

namespace ai_worker {

class Backend {
public:
    virtual ~Backend() = default;
    virtual json Infer(const Frame& frame, const json& options) = 0;
    virtual const char* Name() const = 0;
};

json ObjectFromXywh(int x, int y, int width, int height, float score,
                    int class_id, std::string label,
                    std::string kind = "object");

std::unique_ptr<Backend> CreateMockBackend();
std::unique_ptr<Backend> CreateBackend(const Options& options,
                                       const Manifest& manifest);

}  // namespace ai_worker
