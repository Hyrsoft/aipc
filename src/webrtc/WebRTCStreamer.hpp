#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <rtc/rtc.hpp>

namespace aipc::webrtc {

    class WebRTCStreamer {
    public:
        static WebRTCStreamer &getInstance() {
            static WebRTCStreamer instance;
            return instance;
        }

        ~WebRTCStreamer();

        // Initialize WebRTC with STUN server configuration
        bool init(const std::string &stun_server = "stun:stun.l.google.com:19302");

        // Handle WebRTC Offer from remote peer (typically from Go server)
        // Returns the Answer SDP string, or empty string on error
        std::string handleOffer(const std::string &sdp_offer);

        // Add ICE Candidate from remote peer
        // Called when Go sends candidate information
        bool addCandidate(const std::string &candidate, const std::string &sdp_mid, int sdp_mline_index);

        // Push H.264 frame data to WebRTC track
        // The data should be in Annex-B format (with 00 00 00 01 start codes)
        void pushFrame(const uint8_t *data, size_t size, uint64_t timestamp_ms);

        // Request IDR frame (keyframe) from encoder
        // Call this when a new WebRTC connection is established
        void requestIDR();

        // Check if a peer connection is active
        bool isConnected() const;

        // Cleanup and reset peer connection
        void reset();

    private:
        WebRTCStreamer();

        // Non-copyable, non-movable
        WebRTCStreamer(const WebRTCStreamer &) = delete;
        WebRTCStreamer &operator=(const WebRTCStreamer &) = delete;
        WebRTCStreamer(WebRTCStreamer &&) = delete;
        WebRTCStreamer &operator=(WebRTCStreamer &&) = delete;

        void setupPeerConnectionCallbacks();
        void createVideoTrack();

        std::mutex pc_mutex_;
        std::shared_ptr<rtc::PeerConnection> pc_;
        std::shared_ptr<rtc::Track> video_track_;
        std::atomic<bool> initialized_{false};
        std::atomic<bool> connected_{false};
        std::atomic<bool> answer_ready_{false};
        std::string answer_sdp_;

        // H.264 codec configuration
        static constexpr int VIDEO_PAYLOAD_TYPE = 96;
        static constexpr const char *VIDEO_CODEC = "H264";
    };

} // namespace aipc::webrtc
