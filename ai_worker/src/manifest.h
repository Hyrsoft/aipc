#pragma once

#include <string>

#include "types.h"

namespace ai_worker {

bool SafeName(const std::string& value);
Manifest LoadManifest(const fs::path& project_dir);

}  // namespace ai_worker
