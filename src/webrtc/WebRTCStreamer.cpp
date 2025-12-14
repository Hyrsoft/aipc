#include "WebRTCStreamer.hpp"

#include <spdlog/spdlog.h>
#include <memory>

namespace aipc::webrtc {

    WebRTCStreamer::WebRTCStreamer() = default;

    WebRTCStreamer::~WebRTCStreamer() { reset(); }

    bool WebRTCStreamer::init(const std::string &stun_server) {
        try {
            std::lock_guard<std::mutex> lock(pc_mutex_);

            if (initialized_.load()) {
                SPDLOG_WARN("WebRTCStreamer already initialized");
                return true;
            }

            // Enable libdatachannel logging
            rtc::InitLogger(rtc::LogLevel::Warning);

            // [关键修复] 强制绑定 IPv4，避免 RV1106 报 EAFNOSUPPORT (errno=97) 错误
            rtc::Configuration config;
            config.bindAddress = "0.0.0.0";  // Force IPv4 only
            config.iceServers.emplace_back(stun_server);

            // Test configuration by creating a test PC
            auto test_pc = std::make_shared<rtc::PeerConnection>(config);
            if (!test_pc) {
                SPDLOG_ERROR("Failed to create test PeerConnection with IPv4 config");
                return false;
            }

            initialized_ = true;
            SPDLOG_INFO("WebRTCStreamer initialized successfully with IPv4 binding");
            return true;

        } catch (const std::exception &e) {
            SPDLOG_ERROR("WebRTCStreamer init failed: {}", e.what());
            return false;
        }
    }

    void WebRTCStreamer::createVideoTrack() {
        // Create video description with H.264 codec
        rtc::Description::Video video("video");
        video.addH264Codec(VIDEO_PAYLOAD_TYPE);

        video_track_ = pc_->addTrack(video);
        if (!video_track_) {
            throw std::runtime_error("Failed to add video track");
        }

        SPDLOG_DEBUG("Video track created with H.264 codec");
    }

    void WebRTCStreamer::setupPeerConnectionCallbacks() {
        if (!pc_) {
            return;
        }

        pc_->onStateChange([this](rtc::PeerConnection::State state) {
            const char *state_str[] = {"New", "Connecting", "Connected", "Disconnected", "Failed", "Closed"};
            SPDLOG_INFO("WebRTC PeerConnection state: {}", state_str[static_cast<int>(state)]);

            if (state == rtc::PeerConnection::State::Connected) {
                connected_ = true;
            } else if (state == rtc::PeerConnection::State::Disconnected ||
                       state == rtc::PeerConnection::State::Failed ||
                       state == rtc::PeerConnection::State::Closed) {
                connected_ = false;
            }
        });

        pc_->onLocalDescription([this](rtc::Description description) {
            try {
                answer_sdp_ = std::string(description);
                answer_ready_ = true;
                SPDLOG_INFO("WebRTC Answer ready (length: {})", answer_sdp_.size());
            } catch (const std::exception &e) {
                SPDLOG_ERROR("Error processing local description: {}", e.what());
            }
        });

        pc_->onLocalCandidate([](rtc::Candidate candidate) {
            // For simplicity, we're using ICE gathering complete (full mode)
            // In production, you'd send candidates incrementally
            SPDLOG_DEBUG("ICE candidate: {}", candidate.candidate());
        });

        pc_->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
            const char *state_str[] = {"New", "InProgress", "Complete"};
            SPDLOG_DEBUG("WebRTC ICE gathering state: {}", state_str[static_cast<int>(state)]);
        });
    }

    std::string WebRTCStreamer::handleOffer(const std::string &sdp_offer) {
        try {
            std::lock_guard<std::mutex> lock(pc_mutex_);

            if (!initialized_.load()) {
                SPDLOG_ERROR("WebRTCStreamer not initialized");
                return "";
            }

            // Reset answer flag
            answer_ready_ = false;
            answer_sdp_.clear();

            // Create or reuse PeerConnection
            if (!pc_) {
                rtc::Configuration config;
                config.bindAddress = "0.0.0.0";  // [修复] Force IPv4 only
                config.iceServers.emplace_back("stun:stun.l.google.com:19302");

                pc_ = std::make_shared<rtc::PeerConnection>(config);
                if (!pc_) {
                    throw std::runtime_error("Failed to create PeerConnection");
                }

                setupPeerConnectionCallbacks();
                createVideoTrack();

                SPDLOG_INFO("New PeerConnection created with IPv4 binding");
            }

            // Set remote description (offer)
            try {
                pc_->setRemoteDescription(rtc::Description(sdp_offer, "offer"));
                SPDLOG_INFO("Remote offer set (length: {})", sdp_offer.size());
            } catch (const std::exception &e) {
                SPDLOG_ERROR("Failed to set remote description: {}", e.what());
                return "";
            }

            // Wait for answer to be generated (libdatachannel is async)
            // In practice, you might use a condition variable here for proper sync
            int wait_count = 0;
            while (!answer_ready_.load() && wait_count < 100) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                wait_count++;
            }

            if (!answer_ready_.load()) {
                SPDLOG_WARN("Timeout waiting for answer generation");
                return "";
            }

            SPDLOG_INFO("WebRTC Answer generated successfully (length: {})", answer_sdp_.size());
            return answer_sdp_;

        } catch (const std::exception &e) {
            SPDLOG_ERROR("handleOffer exception: {}", e.what());
            return "";
        }
    }

    void WebRTCStreamer::pushFrame(const uint8_t *data, size_t size, uint64_t timestamp_ms) {
        if (!video_track_ || !pc_ || !connected_.load()) {
            return;
        }

        try {
            std::lock_guard<std::mutex> lock(pc_mutex_);

            if (!video_track_) {
                return;
            }

            // Send frame data (libdatachannel will handle RTP encapsulation)
            video_track_->send(reinterpret_cast<const std::byte *>(data), size);

        } catch (const std::exception &e) {
            SPDLOG_WARN("pushFrame exception: {}", e.what());
        }
    }

    void WebRTCStreamer::requestIDR() {
        // This is a placeholder. In actual implementation, you would call the encoder's IDR request.
        // For now, this is handled in the encoder layer.
        SPDLOG_DEBUG("IDR frame requested");
    }

    bool WebRTCStreamer::isConnected() const { return connected_.load(); }

    void WebRTCStreamer::reset() {
        std::lock_guard<std::mutex> lock(pc_mutex_);

        connected_ = false;
        answer_ready_ = false;
        answer_sdp_.clear();

        if (video_track_) {
            video_track_.reset();
        }

        if (pc_) {
            try {
                pc_->close();
            } catch (const std::exception &e) {
                SPDLOG_WARN("Error closing PeerConnection: {}", e.what());
            }
            pc_.reset();
        }

        SPDLOG_INFO("WebRTCStreamer reset");
    }

    bool WebRTCStreamer::addCandidate(const std::string &candidate, const std::string &sdp_mid,
                                      int sdp_mline_index) {
        try {
            std::lock_guard<std::mutex> lock(pc_mutex_);

            if (!pc_) {
                SPDLOG_WARN("addCandidate called but PeerConnection not initialized");
                return false;
            }

            // Create Candidate object from string
            rtc::Candidate cand(candidate, sdp_mid);

            // Add remote candidate
            pc_->addRemoteCandidate(cand);

            SPDLOG_INFO("Added remote ICE candidate: {} (mid: {})", candidate.substr(0, 80), sdp_mid);
            return true;

        } catch (const std::exception &e) {
            SPDLOG_WARN("Failed to add remote candidate: {}", e.what());
            return false;
        }
    }

} // namespace aipc::webrtc
