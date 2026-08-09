#pragma once

#include <string>
#include <vector>

#include "types.h"

namespace ai_worker {

bool SafeName(const std::string& value);
bool SupportedAlgorithm(const std::string& value);
std::vector<std::string> ReferencedFiles(const Manifest& manifest);
Manifest LoadManifest(const fs::path& project_dir);

}  // namespace ai_worker
