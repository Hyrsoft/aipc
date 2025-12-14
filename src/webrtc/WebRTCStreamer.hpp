#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rtc/rtc.hpp>

namespace aipc::webrtc {

    class WebRTCStreamer {
    public:
        static WebRTCStreamer &get_instance() {
            static WebRTCStreamer instance;
            return instance;
        }

        ~WebRTCStreamer();

        // Initialize WebRTC with STUN server configuration
        bool init(const std::string &stun_server = "stun:stun.l.google.com:19302");

        // Handle WebRTC Offer from remote peer (typically from Go server)
        // Returns the Answer SDP string, or empty string on error
        std::string handle_offer(const std::string &sdp_offer);

        // Add ICE Candidate from remote peer
        // Called when Go sends candidate information
        bool add_candidate(const std::string &candidate, const std::string &sdp_mid, int sdp_mline_index);

        // Push H.264 frame data to WebRTC track
        // The data should be in Annex-B format (with 00 00 00 01 start codes)
        void push_frame(const uint8_t *data, size_t size, uint64_t timestamp_ms);

        // Request IDR frame (keyframe) from encoder
        // Call this when a new WebRTC connection is established
        void request_idr();

        // Check if a peer connection is active
        bool is_connected() const;

        // Cleanup and reset peer connection
        void reset();

    private:
        WebRTCStreamer();

        // Non-copyable, non-movable
        WebRTCStreamer(const WebRTCStreamer &) = delete;
        WebRTCStreamer &operator=(const WebRTCStreamer &) = delete;
        WebRTCStreamer(WebRTCStreamer &&) = delete;
        WebRTCStreamer &operator=(WebRTCStreamer &&) = delete;

        void setup_peer_connection_callbacks();
        bool create_video_track();

        std::mutex pc_mutex_;
        std::shared_ptr<rtc::PeerConnection> pc_;
        std::shared_ptr<rtc::Track> video_track_;
        std::atomic<bool> initialized_{false};
        std::atomic<bool> connected_{false};
        std::atomic<bool> answer_ready_{false};
        std::string answer_sdp_;

        // Signaling sync: build a non-trickle Answer (include all ICE candidates in SDP)
        std::mutex signal_mutex_;
        std::condition_variable signal_cv_;
        std::string pending_local_sdp_;
        bool local_description_received_{false};
        bool gathering_complete_{false};

        struct PendingCandidate {
            std::string candidate;
            std::string sdp_mid;
            int sdp_mline_index{0};
        };
        std::vector<PendingCandidate> pending_candidates_;

        // H.264 codec configuration
        static constexpr int VIDEO_PAYLOAD_TYPE = 96;
        static constexpr const char *VIDEO_CODEC = "H264";
    };

} // namespace aipc::webrtc
