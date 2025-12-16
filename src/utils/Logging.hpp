#pragma once

namespace aipc::logging {

    // Initialize spdlog default logger, pattern and level.
    // Safe to call multiple times.
    void init();

} // namespace aipc::logging
