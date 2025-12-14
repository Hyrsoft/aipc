#pragma once

#include <string>

namespace aipc::webrtc {

    struct SelfTestResult {
        bool ok{false};
        std::string message;
    };

    SelfTestResult run_self_test();

} // namespace aipc::webrtc
