#include "SelfTest.hpp"

#include <memory>

#include <spdlog/spdlog.h>

#include <rtc/rtc.hpp>

namespace aipc::webrtc {

    SelfTestResult RunSelfTest() {
        SPDLOG_INFO("WebRTC self-test starting...");

        try {
            rtc::Configuration config;
            config.iceServers.emplace_back("stun:stun.l.google.com:19302");

            auto pc = std::make_shared<rtc::PeerConnection>(config);

            pc->onStateChange([](rtc::PeerConnection::State state) {
                SPDLOG_DEBUG("WebRTC State: {}", static_cast<int>(state));
            });

            auto dc = pc->createDataChannel("test-channel");
            dc->onOpen([]() { SPDLOG_INFO("DataChannel is OPEN!"); });

            return SelfTestResult{true, "WebRTC Library initialized successfully"};
        } catch (const std::exception &e) {
            return SelfTestResult{false, std::string("WebRTC Error: ") + e.what()};
        }
    }

} // namespace aipc::webrtc
