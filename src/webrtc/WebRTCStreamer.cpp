#include "WebRTCStreamer.hpp"

#include <spdlog/spdlog.h>
#include <chrono>
#include <memory>
#include <thread>

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

            // 强制绑定 IPv4
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
                const std::string sdp = std::string(description);

                {
                    std::lock_guard<std::mutex> lock(signal_mutex_);
                    pending_local_sdp_ = sdp;
                    local_description_received_ = true;
                    if (gathering_complete_) {
                        answer_sdp_ = pending_local_sdp_;
                        answer_ready_.store(true, std::memory_order_release);
                    }
                }
                signal_cv_.notify_all();

                SPDLOG_INFO("WebRTC local description generated (len: {})", sdp.size());
            } catch (const std::exception &e) {
                SPDLOG_ERROR("Error processing local description: {}", e.what());
            }
        });

        pc_->onLocalCandidate([](rtc::Candidate candidate) {
            // Non-trickle mode: we do not push candidates out-of-band.
            // We'll wait for ICE gathering to complete and return an SDP answer containing a=candidate lines.
            SPDLOG_DEBUG("Local ICE candidate gathered: {}", candidate.candidate());
        });

        pc_->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
            const char *state_str[] = {"New", "InProgress", "Complete"};
            SPDLOG_DEBUG("WebRTC ICE gathering state: {}", state_str[static_cast<int>(state)]);

            if (state == rtc::PeerConnection::GatheringState::Complete) {
                {
                    std::lock_guard<std::mutex> lock(signal_mutex_);
                    gathering_complete_ = true;
                    if (local_description_received_) {
                        answer_sdp_ = pending_local_sdp_;
                        answer_ready_.store(true, std::memory_order_release);
                    }
                }
                signal_cv_.notify_all();
            }
        });
    }

    std::string WebRTCStreamer::handleOffer(const std::string &sdp_offer) {
        try {
            if (!initialized_.load()) {
                SPDLOG_ERROR("WebRTCStreamer not initialized");
                return "";
            }

            // Reset answer/gathering flags
            answer_ready_.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(signal_mutex_);
                answer_sdp_.clear();
                pending_local_sdp_.clear();
                local_description_received_ = false;
                gathering_complete_ = false;
            }

            {
                std::lock_guard<std::mutex> lock(pc_mutex_);

                // Always rebuild PeerConnection per offer.
                // Reason: libdatachannel may not support ICE restart; reusing an old PC can fail on refresh/reconnect.
                if (pc_) {
                    SPDLOG_INFO("Rebuilding PeerConnection for new offer");
                    try {
                        pc_->close();
                    } catch (const std::exception &e) {
                        SPDLOG_WARN("Error closing old PeerConnection: {}", e.what());
                    }
                    video_track_.reset();
                    pc_.reset();
                    connected_ = false;
                }

                rtc::Configuration config;
                config.bindAddress = "0.0.0.0";  // [修复] Force IPv4 only
                config.iceServers.emplace_back("stun:stun.l.google.com:19302");

                pc_ = std::make_shared<rtc::PeerConnection>(config);
                if (!pc_) {
                    throw std::runtime_error("Failed to create PeerConnection");
                }

                setupPeerConnectionCallbacks();
                createVideoTrack();

                // Apply any buffered remote candidates (race: candidates arrive before offer/pc creation)
                if (!pending_candidates_.empty()) {
                    SPDLOG_INFO("Applying {} buffered ICE candidates", pending_candidates_.size());
                    for (const auto &pending : pending_candidates_) {
                        try {
                            const std::string mid = pending.sdp_mid.empty() ? "video" : pending.sdp_mid;
                            rtc::Candidate cand(pending.candidate, mid);
                            pc_->addRemoteCandidate(cand);
                        } catch (const std::exception &e) {
                            SPDLOG_WARN("Failed to apply buffered candidate: {}", e.what());
                        }
                    }
                    pending_candidates_.clear();
                }

                SPDLOG_INFO("New PeerConnection created with IPv4 binding");

            // Set remote description (offer)
            try {
                pc_->setRemoteDescription(rtc::Description(sdp_offer, "offer"));
                SPDLOG_INFO("Remote offer set (length: {})", sdp_offer.size());
            } catch (const std::exception &e) {
                SPDLOG_ERROR("Failed to set remote description: {}", e.what());
                return "";
            }
            }

            // Wait for non-trickle answer: local description generated + ICE gathering complete.
            std::string answer;
            {
                std::unique_lock<std::mutex> lock(signal_mutex_);
                const bool ok = signal_cv_.wait_for(lock, std::chrono::seconds(8), [this]() {
                    return answer_ready_.load(std::memory_order_acquire);
                });
                if (!ok) {
                    SPDLOG_WARN("Timeout waiting for ICE gathering complete; answer may be incomplete");
                    // Fallback: if we have a local description, return it anyway.
                    if (!pending_local_sdp_.empty()) {
                        answer = pending_local_sdp_;
                    } else {
                        return "";
                    }
                } else {
                    answer = answer_sdp_;
                }
            }

            const bool has_candidate = (answer.find("a=candidate:") != std::string::npos);
            SPDLOG_INFO("WebRTC Answer ready (len: {}, contains a=candidate: {})", answer.size(), has_candidate);
            SPDLOG_DEBUG("Answer SDP head: {}", answer.substr(0, std::min<size_t>(answer.size(), 600)));
            return answer;

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
        {
            std::lock_guard<std::mutex> sig_lock(signal_mutex_);
            answer_sdp_.clear();
            pending_local_sdp_.clear();
            local_description_received_ = false;
            gathering_complete_ = false;
        }

        pending_candidates_.clear();

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
                PendingCandidate pending;
                pending.candidate = candidate;
                pending.sdp_mid = sdp_mid;
                pending.sdp_mline_index = sdp_mline_index;
                pending_candidates_.push_back(std::move(pending));
                SPDLOG_INFO("Buffered remote ICE candidate (pc not ready yet). buffered={}", pending_candidates_.size());
                return true;
            }

            // Create Candidate object from string
            const std::string mid = sdp_mid.empty() ? "video" : sdp_mid;
            rtc::Candidate cand(candidate, mid);

            // Add remote candidate
            pc_->addRemoteCandidate(cand);

            SPDLOG_INFO("Added remote ICE candidate: {} (mid: {})", candidate.substr(0, 80), mid);
            return true;

        } catch (const std::exception &e) {
            SPDLOG_WARN("Failed to add remote candidate: {}", e.what());
            return false;
        }
    }

} // namespace aipc::webrtc
