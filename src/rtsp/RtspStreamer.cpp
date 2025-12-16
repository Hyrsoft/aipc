#include "RtspStreamer.hpp"

#include <spdlog/spdlog.h>

namespace aipc::rtsp {

    RtspStreamer::RtspStreamer(int port, std::string path) : port_(port), path_(std::move(path)) {
        demo_ = create_rtsp_demo(port_);
        if (!demo_) {
            SPDLOG_ERROR("RTSP create_rtsp_demo failed");
            return;
        }

        session_ = rtsp_new_session(demo_, path_.c_str());
        if (!session_) {
            SPDLOG_ERROR("RTSP rtsp_new_session failed");
            return;
        }
    }

    RtspStreamer::~RtspStreamer() {
        if (session_) {
            rtsp_del_session(session_);
            session_ = nullptr;
        }
        if (demo_) {
            rtsp_del_demo(demo_);
            demo_ = nullptr;
        }
    }

    bool RtspStreamer::ok() const { return demo_ != nullptr && session_ != nullptr; }

    bool RtspStreamer::start_h264() {
        if (!ok()) {
            return false;
        }

        rtsp_set_video(session_, RTSP_CODEC_ID_VIDEO_H264, nullptr, 0);
        rtsp_sync_video_ts(session_, rtsp_get_reltime(), rtsp_get_ntptime());
        return true;
    }

    void RtspStreamer::push_h264(const uint8_t *frame, int len, uint64_t pts) {
        if (!ok()) {
            return;
        }
        rtsp_tx_video(session_, frame, len, pts);
    }

    void RtspStreamer::poll() {
        if (!demo_) {
            return;
        }
        rtsp_do_event(demo_);
    }

} // namespace aipc::rtsp
