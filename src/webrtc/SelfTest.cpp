#include "SelfTest.hpp"

#include <iostream>
#include <memory>

#include <rtc/rtc.hpp>

namespace aipc::webrtc {

    SelfTestResult RunSelfTest() {
        std::cout << "[aipc] WebRTC self-test starting..." << std::endl;

        try {
            rtc::Configuration config;
            config.iceServers.emplace_back("stun:stun.l.google.com:19302");

            auto pc = std::make_shared<rtc::PeerConnection>(config);

            pc->onStateChange([](rtc::PeerConnection::State state) {
                std::cout << "[aipc] WebRTC State: " << static_cast<int>(state) << std::endl;
            });

            auto dc = pc->createDataChannel("test-channel");
            dc->onOpen([]() { std::cout << "[aipc] DataChannel is OPEN!" << std::endl; });

            return SelfTestResult{true, "WebRTC Library initialized successfully"};
        } catch (const std::exception &e) {
            return SelfTestResult{false, std::string("WebRTC Error: ") + e.what()};
        }
    }

} // namespace aipc::webrtc
