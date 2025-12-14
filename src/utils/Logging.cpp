#include "Logging.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <mutex>
#include <string>

namespace aipc::logging {

void Init() {
    static std::once_flag once;
    std::call_once(once, []() {
        try {
            auto logger = spdlog::stdout_color_mt("aipc");
            spdlog::set_default_logger(std::move(logger));

            // time, level, logger-name, message
            spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");

#ifndef NDEBUG
            spdlog::set_level(spdlog::level::debug);
#else
            spdlog::set_level(spdlog::level::info);
#endif

            if (const char *env_level = std::getenv("AIPC_LOG_LEVEL")) {
                std::string level_str(env_level);
                if (!level_str.empty()) {
                    spdlog::set_level(spdlog::level::from_str(level_str));
                }
            }

            spdlog::flush_on(spdlog::level::info);
        } catch (...) {
            // If spdlog init fails, keep running with whatever default behaviour exists.
        }
    });
}

} // namespace aipc::logging
