#include "WebRTCStreamer.hpp"

#include <chrono>
#include <memory>
#include <spdlog/spdlog.h>
#include <thread>
#include <nlohmann/json.hpp>

namespace aipc::webrtc {

    WebRTCStreamer::WebRTCStreamer() = default;

    WebRTCStreamer::~WebRTCStreamer() { reset(); }

    bool WebRTCStreamer::init(const std::string &stun_server) {
        std::lock_guard<std::mutex> lock(pc_mutex_);

        if (initialized_.load()) {
            SPDLOG_WARN("WebRTCStreamer already initialized");
            return true;
        }

        rtc::InitLogger(rtc::LogLevel::Warning);

        // We don't create PC here anymore, we create it on offer
        initialized_ = true;
        SPDLOG_INFO("WebRTCStreamer initialized successfully");
        return true;
    }

    void WebRTCStreamer::start_server(uint16_t port) {
        rtc::WebSocketServer::Configuration config;
        config.port = port;
        config.bindAddress = "0.0.0.0";
        
        try {
            ws_server_ = std::make_shared<rtc::WebSocketServer>(config);
        } catch (const std::exception &e) {
            SPDLOG_ERROR("Failed to start WebSocket server on port {}: {}", port, e.what());
            return;
        }
        
        ws_server_->onClient([this](std::shared_ptr<rtc::WebSocket> ws) {
            SPDLOG_INFO("WebSocket client connected");
            std::lock_guard<std::mutex> lock(ws_mutex_);
            ws_client_ = ws;
            
            ws->onMessage([this, ws](auto message) {
                if (std::holds_alternative<std::string>(message)) {
                    std::string msg = std::get<std::string>(message);
                    try {
                        auto json = nlohmann::json::parse(msg);
                        std::string type = json.value("type", "");
                        
                        if (type == "offer") {
                            std::string sdp = json.value("sdp", "");
                            std::string answer = handle_offer(sdp);
                            if (!answer.empty()) {
                                nlohmann::json resp;
                                resp["type"] = "answer";
                                resp["sdp"] = answer;
                                ws->send(resp.dump());
                            }
                        } else if (type == "candidate") {
                            std::string cand = json.value("candidate", "");
                            std::string mid = json.value("sdpMid", "");
                            int mline = json.value("sdpMLineIndex", 0);
                            add_candidate(cand, mid, mline);
                        } else if (type == "control") {
                            if (control_cb_) {
                                control_cb_(msg);
                            }
                        }
                    } catch (const std::exception &e) {
                        SPDLOG_ERROR("JSON parse error: {}", e.what());
                    }
                }
            });
            
            ws->onClosed([this]() {
                SPDLOG_INFO("WebSocket client disconnected");
            });
        });
        SPDLOG_INFO("WebSocket server started on port {}", port);
    }

    void WebRTCStreamer::send_data(const std::string &data) {
        if (data_channel_ && data_channel_->isOpen()) {
            data_channel_->send(data);
        } else {
            std::lock_guard<std::mutex> lock(ws_mutex_);
            if (ws_client_) {
                ws_client_->send(data);
            }
        }
    }

    bool WebRTCStreamer::create_video_track() {
        if (!pc_) {
            SPDLOG_ERROR("create_video_track called without PeerConnection");
            return false;
        }

        // Create video description with H.264 codec
        rtc::Description::Video video("video");
        video.addH264Codec(VIDEO_PAYLOAD_TYPE);

        video_track_ = pc_->addTrack(video);
        if (!video_track_) {
            SPDLOG_ERROR("Failed to add video track");
            return false;
        }

        SPDLOG_DEBUG("Video track created with H.264 codec");
        return true;
    }

    void WebRTCStreamer::setup_peer_connection_callbacks() {
        if (!pc_) {
            return;
        }

        pc_->onStateChange([this](rtc::PeerConnection::State state) {
            const char *state_str[] = {"New", "Connecting", "Connected", "Disconnected", "Failed", "Closed"};
            SPDLOG_INFO("WebRTC PeerConnection state: {}", state_str[static_cast<int>(state)]);

            if (state == rtc::PeerConnection::State::Connected) {
                connected_ = true;
            } else if (state == rtc::PeerConnection::State::Disconnected ||
                       state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed) {
                connected_ = false;
            }
        });

        pc_->onLocalDescription([this](rtc::Description description) {
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

        pc_->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
            SPDLOG_INFO("DataChannel received: {}", dc->label());
            data_channel_ = dc;
            
            dc->onMessage([this](auto message) {
                if (std::holds_alternative<std::string>(message)) {
                    std::string msg = std::get<std::string>(message);
                    if (control_cb_) {
                        control_cb_(msg);
                    }
                }
            });
        });
    }

    std::string WebRTCStreamer::handle_offer(const std::string &sdp_offer) {
        if (!initialized_.load()) {
            SPDLOG_ERROR("WebRTCStreamer not initialized");
            return {};
        }

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

            if (pc_) {
                SPDLOG_INFO("Rebuilding PeerConnection for new offer");
                pc_->close();
                video_track_.reset();
                pc_.reset();
                connected_ = false;
            }

            rtc::Configuration config;
            config.bindAddress = "0.0.0.0";
            config.iceServers.emplace_back("stun:stun.l.google.com:19302");

            pc_ = std::make_shared<rtc::PeerConnection>(config);
            if (!pc_) {
                SPDLOG_ERROR("Failed to create PeerConnection");
                return {};
            }

            setup_peer_connection_callbacks();
            if (!create_video_track()) {
                return {};
            }

            if (!pending_candidates_.empty()) {
                SPDLOG_INFO("Applying {} buffered ICE candidates", pending_candidates_.size());
                for (const auto &pending: pending_candidates_) {
                    const std::string mid = pending.sdp_mid.empty() ? "video" : pending.sdp_mid;
                    rtc::Candidate cand(pending.candidate, mid);
                    pc_->addRemoteCandidate(cand);
                }
                pending_candidates_.clear();
            }

            SPDLOG_INFO("New PeerConnection created with IPv4 binding");

            pc_->setRemoteDescription(rtc::Description(sdp_offer, "offer"));
            SPDLOG_INFO("Remote offer set (length: {})", sdp_offer.size());
        }

        std::string answer;
        {
            std::unique_lock<std::mutex> lock(signal_mutex_);
            const bool ok = signal_cv_.wait_for(lock, std::chrono::seconds(8),
                                                [this]() { return answer_ready_.load(std::memory_order_acquire); });
            if (!ok) {
                SPDLOG_WARN("Timeout waiting for ICE gathering complete; answer may be incomplete");
                if (!pending_local_sdp_.empty()) {
                    answer = pending_local_sdp_;
                } else {
                    return {};
                }
            } else {
                answer = answer_sdp_;
            }
        }

        const bool has_candidate = (answer.find("a=candidate:") != std::string::npos);
        SPDLOG_INFO("WebRTC Answer ready (len: {}, contains a=candidate: {})", answer.size(), has_candidate);
        SPDLOG_DEBUG("Answer SDP head: {}", answer.substr(0, std::min<size_t>(answer.size(), 600)));
        return answer;
    }

    void WebRTCStreamer::push_frame(const uint8_t *data, size_t size, uint64_t timestamp_ms) {
        if (!video_track_ || !pc_ || !connected_.load()) {
            return;
        }

        std::lock_guard<std::mutex> lock(pc_mutex_);
        if (!video_track_) {
            return;
        }

        (void) timestamp_ms;
        video_track_->send(reinterpret_cast<const std::byte *>(data), size);
    }

    void WebRTCStreamer::request_idr() {
        // This is a placeholder. In actual implementation, you would call the encoder's IDR request.
        // For now, this is handled in the encoder layer.
        SPDLOG_DEBUG("IDR frame requested");
    }

    bool WebRTCStreamer::is_connected() const { return connected_.load(); }

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
            pc_->close();
            pc_.reset();
        }

        SPDLOG_INFO("WebRTCStreamer reset");
    }

    bool WebRTCStreamer::add_candidate(const std::string &candidate, const std::string &sdp_mid, int sdp_mline_index) {
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

        const std::string mid = sdp_mid.empty() ? "video" : sdp_mid;
        rtc::Candidate cand(candidate, mid);
        pc_->addRemoteCandidate(cand);
        SPDLOG_INFO("Added remote ICE candidate: {} (mid: {})", candidate.substr(0, 80), mid);
        return true;
    }

} // namespace aipc::webrtc
