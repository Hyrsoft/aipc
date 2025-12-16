#include <assert.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>
#include "ai/AIManager.hpp"
// #include "comm/CommandListener.hpp"
#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "ai/osd/Osd.hpp"
#include "rkmpi/IspSession.hpp"
#include "rkmpi/MbBlock.hpp"
#include "rkmpi/MbPool.hpp"
#include "rkmpi/MpiSystem.hpp"
#include "rkmpi/Time.hpp"
#include "rtsp/RtspStreamer.hpp"
#include "utils/Logging.hpp"
#include "video/VencSession.hpp"
#include "video/ViSession.hpp"
#include "webrtc/WebRTCStreamer.hpp"

namespace {

    // 全局变量，判断程序是否在运行
    std::atomic<bool> g_running{true};

    void HandleSignal(int) { g_running = false; }

    struct AppConfig {
        int vi_dev{0};
        int vi_chn{0};

        int width{720};
        int height{480};

        int command_port{9000};
        int rtsp_port{554};
        const char *rtsp_path{"/live/0"};

        const char *iq_dir{"/etc/iqfiles"};
        RK_BOOL multi_sensor{RK_FALSE};
        rk_aiq_working_mode_t hdr_mode{RK_AIQ_WORKING_MODE_NORMAL};

        const char *default_model_path{"./model/yolov5.rknn"};
    };

    struct ViFrameGuard {
        int dev{0};
        int chn{0};
        VIDEO_FRAME_INFO_S *frame{nullptr};
        bool active{false};

        // 在析构函数中释放资源
        ~ViFrameGuard() {
            if (!active || !frame) {
                return;
            }
            const auto ret = RK_MPI_VI_ReleaseChnFrame(dev, chn, frame);
            if (ret != RK_SUCCESS) {
                SPDLOG_WARN("RK_MPI_VI_ReleaseChnFrame fail {:x}", static_cast<unsigned int>(ret));
            }
        }
    };

    struct VencStreamGuard {
        int chn{0};
        VENC_STREAM_S *stream{nullptr};
        bool active{false};

        ~VencStreamGuard() {
            if (!active || !stream) {
                return;
            }
            const auto ret = RK_MPI_VENC_ReleaseStream(chn, stream);
            if (ret != RK_SUCCESS) {
                SPDLOG_WARN("RK_MPI_VENC_ReleaseStream fail {:x}", static_cast<unsigned int>(ret));
            }
        }
    };

} // namespace

int main(int argc, char *argv[]) {

    // 初始化日志
    aipc::logging::init();

    (void) argc;
    (void) argv;

    const AppConfig cfg;

    ::signal(SIGINT, HandleSignal);
    ::signal(SIGTERM, HandleSignal);

    // Initialize WebRTC Streamer
    auto &webrtc_streamer = aipc::webrtc::WebRTCStreamer::get_instance();
    if (!webrtc_streamer.init()) {
        SPDLOG_ERROR("Failed to initialize WebRTC Streamer");
        return -1;
    }
    SPDLOG_INFO("WebRTC Streamer 初始化成功");

    // 清理默认的ipc进程
    system("RkLunch-stop.sh");

    RK_S32 s32Ret = 0;

    // Initialize AI Manager (default: YOLOv5)
    aipc::ai::AIManager::get_instance().switch_model(aipc::ai::ModelType::YOLOV5, cfg.default_model_path);

    // 启动命令监听（UDP:9000）
    // Now supports JSON format: {"type": "webrtc_offer", "payload": "v=0\r\no=- ..."}
    // Set up control callback for WebRTC
    webrtc_streamer.set_control_callback([](const std::string &msg_str) {
        try {
            auto json = nlohmann::json::parse(msg_str);
            std::string type = json.value("type", "");
            
            if (type == "control") {
                std::string cmd = json.value("command", "");
                SPDLOG_INFO("Processing control command: {}", cmd);

                if (cmd == "model_yolov5") {
                    aipc::ai::AIManager::get_instance().switch_model(aipc::ai::ModelType::YOLOV5, "./model/yolov5.rknn");
                    SPDLOG_INFO("Switched to YOLOV5 model");
                } else if (cmd == "model_retinaface") {
                    aipc::ai::AIManager::get_instance().switch_model(aipc::ai::ModelType::RETINAFACE, "./model/retinaface.rknn");
                    SPDLOG_INFO("Switched to RETINAFACE model");
                } else if (cmd == "model_none") {
                    aipc::ai::AIManager::get_instance().switch_model(aipc::ai::ModelType::NONE);
                    SPDLOG_INFO("Switched to NONE model");
                }
            }
        } catch (const std::exception &e) {
            SPDLOG_ERROR("Control callback error: {}", e.what());
        }
    });

    // Start WebSocket server for signaling and control
    webrtc_streamer.start_server(8000);

    const int width = cfg.width;
    const int height = cfg.height;

    // ISP Init
    aipc::rkmpi::IspSession isp(0, cfg.hdr_mode, cfg.multi_sensor, cfg.iq_dir);

    // MPI Init
    aipc::rkmpi::MpiSystem mpi;
    if (!mpi.ok()) {
        return -1;
    }

    // Create RGB pool
    auto rgb_pool = aipc::rkmpi::MbPool::create(static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 3u, 4);
    if (!rgb_pool.ok()) {
        return -1;
    }

    // H264 stream pack buffer
    auto venc_delete = [](VENC_PACK_S *p) { std::free(p); };

    auto pack = std::unique_ptr<VENC_PACK_S, decltype(venc_delete)>(
            static_cast<VENC_PACK_S *>(std::malloc(sizeof(VENC_PACK_S))), venc_delete);

    if (!pack) {
        SPDLOG_ERROR("malloc VENC_PACK_S failed");
        return -1;
    }

    VENC_STREAM_S stFrame;
    std::memset(&stFrame, 0, sizeof(stFrame));
    stFrame.pstPack = pack.get();

    RK_U32 H264_TimeRef = 0;
    VIDEO_FRAME_INFO_S stViFrame;

    // Build h264_frame structure (common parts)
    VIDEO_FRAME_INFO_S h264_frame;
    std::memset(&h264_frame, 0, sizeof(h264_frame));
    h264_frame.stVFrame.u32Width = width;
    h264_frame.stVFrame.u32Height = height;
    h264_frame.stVFrame.u32VirWidth = width;
    h264_frame.stVFrame.u32VirHeight = height;
    h264_frame.stVFrame.enPixelFormat = RK_FMT_RGB888;
    h264_frame.stVFrame.u32FrameFlag = 160;

    // RTSP Init
    aipc::rtsp::RtspStreamer rtsp_streamer(cfg.rtsp_port, cfg.rtsp_path);
    if (!rtsp_streamer.ok() || !rtsp_streamer.start_h264()) {
        SPDLOG_ERROR("RTSP init failed");
        return -1;
    }

    // VI/VENC Init
    aipc::vi::ViSession vi(cfg.vi_dev, cfg.vi_chn, width, height);
    if (!vi.ok()) {
        return -1;
    }

    aipc::venc::VencSession venc(0, width, height, RK_VIDEO_ID_AVC);
    if (!venc.ok()) {
        return -1;
    }

    SPDLOG_INFO("VI/VENC init success");

    // AI async worker: consume latest frame, run inference, update latest results
    // AI推理线程，生产最新的视频帧，更新结果
    std::mutex ai_m;
    std::condition_variable ai_cv;
    std::shared_ptr<aipc::rkmpi::MbBlock> ai_latest_blk;
    bool ai_stop = false;

    std::mutex results_m;
    std::vector<aipc::ai::ObjectDet> latest_results;

    std::thread ai_thread([&]() {
        std::vector<aipc::ai::ObjectDet> local_results; // AI推理结果
        while (true) {
            std::shared_ptr<aipc::rkmpi::MbBlock> blk;
            {
                std::unique_lock<std::mutex> lk(ai_m);
                ai_cv.wait(lk, [&]() { return ai_stop || ai_latest_blk != nullptr; });
                if (ai_stop) {
                    break;
                }
                blk = std::move(ai_latest_blk);
                ai_latest_blk.reset();
            }

            if (!blk || !blk->ok()) {
                continue;
            }

            cv::Mat bgr(height, width, CV_8UC3, blk->vir_addr());
            cv::Mat bgr_copy = bgr.clone();
            blk.reset();

            local_results.clear();
            aipc::ai::AIManager::get_instance().run_inference(bgr_copy, local_results);

            {
                std::lock_guard<std::mutex> lk(results_m);
                latest_results = local_results;
            }
        }
    });


    while (g_running.load()) {
        // Get RGB MB from pool for this frame
        auto rgb_blk = aipc::rkmpi::MbBlock::get(rgb_pool.get(),
                                                 static_cast<size_t>(width) * static_cast<size_t>(height) * 3u, true);
        if (!rgb_blk || !rgb_blk->ok()) {
            continue;
        }
        unsigned char *rgb_data = static_cast<unsigned char *>(rgb_blk->vir_addr());
        if (!rgb_data) {
            continue;
        }
        h264_frame.stVFrame.pMbBlk = rgb_blk->handle();

        // Timestamp
        h264_frame.stVFrame.u32TimeRef = H264_TimeRef++;
        h264_frame.stVFrame.u64PTS = aipc::rkmpi::now_us();

        // Get VI Frame
        s32Ret = RK_MPI_VI_GetChnFrame(cfg.vi_dev, cfg.vi_chn, &stViFrame, -1);
        ViFrameGuard vi_guard{cfg.vi_dev, cfg.vi_chn, &stViFrame, s32Ret == RK_SUCCESS};
        if (s32Ret != RK_SUCCESS) {
            continue;
        }

        void *vi_data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);
        if (!vi_data) {
            continue;
        }

        cv::Mat yuv420sp(height + height / 2, width, CV_8UC1, vi_data);
        cv::Mat bgr(height, width, CV_8UC3, rgb_data);
        cv::cvtColor(yuv420sp, bgr, cv::COLOR_YUV420sp2BGR);

        // Draw latest results (may be from older frame)
        std::vector<aipc::ai::ObjectDet> draw_results;
        {
            std::lock_guard<std::mutex> lk(results_m);
            draw_results = latest_results;
        }
        aipc::osd::draw_detections(bgr, draw_results);

        // Submit latest frame to AI thread (drop old if not consumed)
        // 注意：放在 OSD 之后，避免 AI 读/主线程写同一缓冲的竞争。
        {
            std::lock_guard<std::mutex> lk(ai_m);
            ai_latest_blk = rgb_blk;
        }
        ai_cv.notify_one();

        // Encode H264
        s32Ret = RK_MPI_VENC_SendFrame(0, &h264_frame, -1);
        if (s32Ret != RK_SUCCESS) {
            SPDLOG_WARN("RK_MPI_VENC_SendFrame fail {:x}", static_cast<unsigned int>(s32Ret));
        }

        // RTSP Send
        s32Ret = RK_MPI_VENC_GetStream(0, &stFrame, -1);
        VencStreamGuard venc_guard{0, &stFrame, s32Ret == RK_SUCCESS};
        if (s32Ret == RK_SUCCESS) {
            void *pData = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
            if (pData) {
                // Send to RTSP
                rtsp_streamer.push_h264((uint8_t *) pData, stFrame.pstPack->u32Len, stFrame.pstPack->u64PTS);
                rtsp_streamer.poll();

                // Send to WebRTC if connected
                if (webrtc_streamer.is_connected()) {
                    webrtc_streamer.push_frame((const uint8_t *) pData, stFrame.pstPack->u32Len,
                                               stFrame.pstPack->u64PTS / 1000); // Convert to ms
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(ai_m);
        ai_stop = true;
    }
    ai_cv.notify_one();
    if (ai_thread.joinable()) {
        ai_thread.join();
    }

    return 0;
}
