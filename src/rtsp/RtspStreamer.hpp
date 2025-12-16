#pragma once

#include <cstdint>
#include <string>

#include "rtsp_demo.h"

namespace aipc::rtsp {

    class RtspStreamer {
    public:
        RtspStreamer(int port, std::string path);
        ~RtspStreamer();

        RtspStreamer(const RtspStreamer &) = delete;
        RtspStreamer &operator=(const RtspStreamer &) = delete;

        bool ok() const;

        // H264: codec_data 为空时将按 demo 默认方式推流（与原 main.cpp 保持一致）
        bool start_h264();

        void push_h264(const uint8_t *frame, int len, uint64_t pts);
        void poll();

    private:
        int port_;
        std::string path_;
        rtsp_demo_handle demo_{nullptr};
        rtsp_session_handle session_{nullptr};
    };

} // namespace aipc::rtsp
