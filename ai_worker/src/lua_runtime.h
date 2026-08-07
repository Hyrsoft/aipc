#pragma once

#include <memory>

#include "backend.h"
#include "types.h"

namespace ai_worker {

class LuaRuntime {
public:
    LuaRuntime(const Manifest& manifest, const fs::path& project_dir,
               std::unique_ptr<Backend> backend);
    ~LuaRuntime();

    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    json Process(const Frame& frame);
    const char* BackendName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ai_worker
