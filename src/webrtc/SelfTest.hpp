#pragma once

#include <string>

namespace aipc::webrtc {

    struct SelfTestResult {
        bool ok{false};
        std::string message;
    };

    SelfTestResult RunSelfTest();

} // namespace aipc::webrtc
