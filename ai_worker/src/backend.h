#pragma once

#include <memory>
#include <vector>

#include "types.h"

namespace ai_worker {

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::vector<DetectionResult> Infer(const Frame& frame) = 0;
    virtual const char* Name() const = 0;
};

std::unique_ptr<Backend> CreateMockBackend();
std::unique_ptr<Backend> CreateBackend(const Options& options,
                                       const Manifest& manifest);

}  // namespace ai_worker
