#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "rkmpi/IspSession.hpp"
#include "rkmpi/MbBlock.hpp"
#include "rkmpi/MbPool.hpp"
#include "rkmpi/MpiSystem.hpp"
#include "rkmpi/Time.hpp"

#include "vi/ViSession.hpp"

#include "comm/CommandListener.hpp"
#include "webrtc/SelfTest.hpp"
#include "rtsp/RtspStreamer.hpp"

#include "ai/AIManager.hpp"

#include "osd/Osd.hpp"
#include "utils/Logging.hpp"

#include "venc/VencSession.hpp"

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"

namespace {

std::atomic<bool> g_running{true};

void HandleSignal(int) { g_running = false; }

struct ViFrameGuard {
    int dev{0};
    int chn{0};
    VIDEO_FRAME_INFO_S *frame{nullptr};
    bool active{false};

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
    aipc::logging::Init();

    ::signal(SIGINT, HandleSignal);
    ::signal(SIGTERM, HandleSignal);

    const auto webrtc_test = aipc::webrtc::RunSelfTest();
    if (!webrtc_test.ok) {
        SPDLOG_ERROR("{}", webrtc_test.message);
        return -1;
    }
    SPDLOG_INFO("{}", webrtc_test.message);

    // Stop existing rkipc or other processes if needed
    // 清理默认的ipc进程
    system("RkLunch-stop.sh");

    RK_S32 s32Ret = 0;
    
    // Initialize AI Manager
    // Default to YOLOv5
    aipc::ai::AIManager::Instance().SwitchModel(aipc::ai::ModelType::YOLOV5, "./model/yolov5.rknn");

    // 启动命令监听（UDP:9000）
    aipc::comm::CommandListener command_listener(9000, [](const std::string& cmd) {
        SPDLOG_INFO("Received command: {}", cmd);

        if (cmd.find("YOLOV5") != std::string::npos || cmd.find("yolov5") != std::string::npos) {
            aipc::ai::AIManager::Instance().SwitchModel(aipc::ai::ModelType::YOLOV5, "./model/yolov5.rknn");
        } else if (cmd.find("RETINAFACE") != std::string::npos || cmd.find("retinaface") != std::string::npos) {
            aipc::ai::AIManager::Instance().SwitchModel(aipc::ai::ModelType::RETINAFACE, "./model/retinaface.rknn");
        } else if (cmd.find("NONE") != std::string::npos || cmd.find("none") != std::string::npos) {
            aipc::ai::AIManager::Instance().SwitchModel(aipc::ai::ModelType::NONE);
        }
    });
    command_listener.start();

    constexpr int width = 720;
    constexpr int height = 480;

    // ISP Init (RAII)
    RK_BOOL multi_sensor = RK_FALSE;
    const char *iq_dir = "/etc/iqfiles";
    rk_aiq_working_mode_t hdr_mode = RK_AIQ_WORKING_MODE_NORMAL;
    aipc::rkmpi::IspSession isp(0, hdr_mode, multi_sensor, iq_dir);

    // MPI Init (RAII)
    aipc::rkmpi::MpiSystem mpi;
    if (!mpi.ok()) {
        return -1;
    }

    // Create RGB pool (RAII)
    auto rgb_pool = aipc::rkmpi::MbPool::Create(static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 3u, 4);
    if (!rgb_pool.ok()) {
        return -1;
    }

    // H264 stream pack buffer (RAII)
    auto pack = std::unique_ptr<VENC_PACK_S, void (*)(void *)>(
            static_cast<VENC_PACK_S *>(std::malloc(sizeof(VENC_PACK_S))), std::free);
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
    aipc::rtsp::RtspStreamer rtsp_streamer(554, "/live/0");
    if (!rtsp_streamer.ok() || !rtsp_streamer.startH264()) {
        SPDLOG_ERROR("RTSP init failed");
        return -1;
    }

    // VI/VENC Init (RAII)
    aipc::vi::ViSession vi(0, 0, width, height);
    if (!vi.ok()) {
        return -1;
    }

    aipc::venc::VencSession venc(0, width, height, RK_VIDEO_ID_AVC);
    if (!venc.ok()) {
        return -1;
    }

    SPDLOG_INFO("VI/VENC init success");
    
    // AI async worker: consume latest frame, run inference, update latest results
    std::mutex ai_m;
    std::condition_variable ai_cv;
    std::shared_ptr<aipc::rkmpi::MbBlock> ai_latest_blk;
    bool ai_stop = false;

    std::mutex results_m;
    std::vector<aipc::ai::ObjectDet> latest_results;

    std::thread ai_thread([&]() {
        std::vector<aipc::ai::ObjectDet> local_results;
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

            cv::Mat bgr(height, width, CV_8UC3, blk->virAddr());
            cv::Mat bgr_copy = bgr.clone();
            blk.reset();

            local_results.clear();
            aipc::ai::AIManager::Instance().RunInference(bgr_copy, local_results);

            {
                std::lock_guard<std::mutex> lk(results_m);
                latest_results = local_results;
            }
        }
    });

    while (g_running.load()) {
        // Get RGB MB from pool for this frame
        auto rgb_blk = aipc::rkmpi::MbBlock::Get(rgb_pool.get(), static_cast<size_t>(width) * static_cast<size_t>(height) * 3u, true);
        if (!rgb_blk || !rgb_blk->ok()) {
            continue;
        }
        unsigned char *rgb_data = static_cast<unsigned char *>(rgb_blk->virAddr());
        if (!rgb_data) {
            continue;
        }
        h264_frame.stVFrame.pMbBlk = rgb_blk->handle();

        // Timestamp
        h264_frame.stVFrame.u32TimeRef = H264_TimeRef++;
        h264_frame.stVFrame.u64PTS = aipc::rkmpi::NowUs();

        // Get VI Frame
        s32Ret = RK_MPI_VI_GetChnFrame(0, 0, &stViFrame, -1);
        ViFrameGuard vi_guard{0, 0, &stViFrame, s32Ret == RK_SUCCESS};
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
        aipc::osd::DrawDetections(bgr, draw_results);

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
                rtsp_streamer.pushH264((uint8_t *) pData, stFrame.pstPack->u32Len, stFrame.pstPack->u64PTS);
                rtsp_streamer.poll();
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
