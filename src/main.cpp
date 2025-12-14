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
#include "webrtc/WebRTCStreamer.hpp"
#include "rtsp/RtspStreamer.hpp"

#include <nlohmann/json.hpp>

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

    (void) argc;
    (void) argv;

    const AppConfig cfg;

    ::signal(SIGINT, HandleSignal);
    ::signal(SIGTERM, HandleSignal);

    const auto webrtc_test = aipc::webrtc::RunSelfTest();
    if (!webrtc_test.ok) {
        SPDLOG_ERROR("{}", webrtc_test.message);
        return -1;
    }
    SPDLOG_INFO("{}", webrtc_test.message);

    // Initialize WebRTC Streamer
    auto &webrtc_streamer = aipc::webrtc::WebRTCStreamer::getInstance();
    if (!webrtc_streamer.init()) {
        SPDLOG_ERROR("Failed to initialize WebRTC Streamer");
        return -1;
    }
    SPDLOG_INFO("WebRTC Streamer initialized");

    // Stop existing rkipc or other processes if needed
    // 清理默认的ipc进程
    system("RkLunch-stop.sh");

    RK_S32 s32Ret = 0;
    
    // Initialize AI Manager (default: YOLOv5)
    aipc::ai::AIManager::Instance().SwitchModel(aipc::ai::ModelType::YOLOV5, cfg.default_model_path);

    // 启动命令监听（UDP:9000）
    // Now supports JSON format: {"type": "webrtc_offer", "payload": "v=0\r\no=- ..."}
    aipc::comm::CommandListener command_listener(cfg.command_port, [&](const aipc::comm::CommandMessage &msg) -> std::string {
        SPDLOG_INFO("Received command type: '{}', payload size: {}", msg.type, msg.payload.size());

        // Validate command
        if (msg.type.empty()) {
            SPDLOG_WARN("Empty command type received");
            nlohmann::json error_response;
            error_response["type"] = "error";
            error_response["message"] = "Empty command type";
            return error_response.dump();
        }

        if (msg.type == "webrtc_offer") {
            // Validate payload
            if (msg.payload.empty()) {
                SPDLOG_ERROR("WebRTC offer received with empty payload");
                nlohmann::json error_response;
                error_response["type"] = "error";
                error_response["message"] = "Empty SDP payload";
                return error_response.dump();
            }

            SPDLOG_INFO("Processing WebRTC Offer (payload size: {})", msg.payload.size());
            std::string answer = webrtc_streamer.handleOffer(msg.payload);
            
            if (answer.empty()) {
                SPDLOG_ERROR("Failed to generate WebRTC Answer");
                // Return error response
                nlohmann::json error_response;
                error_response["type"] = "error";
                error_response["message"] = "Failed to generate Answer";
                return error_response.dump();
            }

            // Return Answer wrapped in JSON
            nlohmann::json response;
            response["type"] = "webrtc_answer";
            response["payload"] = answer;
            
            SPDLOG_INFO("WebRTC Answer generated successfully");
            
            // Request IDR frame when new connection is established
            webrtc_streamer.requestIDR();
            
            return response.dump();
        }
        // [新增] 处理 ICE Candidate 交换
        else if (msg.type == "webrtc_candidate") {
            if (msg.payload.empty()) {
                SPDLOG_WARN("WebRTC candidate received with empty payload");
                nlohmann::json error_response;
                error_response["type"] = "error";
                error_response["message"] = "Empty candidate payload";
                return error_response.dump();
            }

            SPDLOG_INFO("Processing WebRTC Candidate");
            try {
                // payload 通常是 JSON({candidate,sdpMid,sdpMLineIndex})；若不是 JSON，则直接把 payload 当 candidate 字符串。
                std::string candidate;
                std::string sdp_mid;
                int sdp_mline_index = 0;

                try {
                    auto payload_json = nlohmann::json::parse(msg.payload);
                    candidate = payload_json.value("candidate", "");
                    sdp_mid = payload_json.value("sdpMid", "");
                    sdp_mline_index = payload_json.value("sdpMLineIndex", 0);
                } catch (...) {
                    candidate = msg.payload;
                }

                if (sdp_mid.empty()) {
                    sdp_mid = "video";
                }
                
                if (candidate.empty()) {
                    SPDLOG_WARN("Candidate payload missing 'candidate' field");
                    nlohmann::json error_response;
                    error_response["type"] = "error";
                    error_response["message"] = "Missing candidate field";
                    return error_response.dump();
                }

                // 将 candidate 添加到 WebRTC 连接
                bool success = webrtc_streamer.addCandidate(candidate, sdp_mid, sdp_mline_index);
                
                nlohmann::json response;
                if (success) {
                    response["type"] = "ack";
                    response["message"] = "Candidate added successfully";
                    SPDLOG_INFO("Successfully added ICE candidate");
                } else {
                    response["type"] = "error";
                    response["message"] = "Failed to add candidate";
                    SPDLOG_WARN("Failed to add ICE candidate");
                }
                return response.dump();
                
            } catch (const std::exception &e) {
                SPDLOG_ERROR("Error processing candidate: {}", e.what());
                nlohmann::json error_response;
                error_response["type"] = "error";
                error_response["message"] = std::string("Candidate processing error: ") + e.what();
                return error_response.dump();
            }
        }
        else if (msg.type == "control_cmd" || msg.type == "model_switch") {
            // Validate payload for model switch
            if ((msg.type == "model_switch") && msg.payload.empty()) {
                SPDLOG_WARN("model_switch received with empty payload");
                nlohmann::json error_response;
                error_response["type"] = "error";
                error_response["message"] = "Empty model_switch payload";
                return error_response.dump();
            }

            const std::string &cmd = msg.payload;
            SPDLOG_INFO("Processing control command: {}", cmd);

            if (cmd.find("YOLOV5") != std::string::npos || cmd.find("yolov5") != std::string::npos) {
                aipc::ai::AIManager::Instance().SwitchModel(aipc::ai::ModelType::YOLOV5, "./model/yolov5.rknn");
                SPDLOG_INFO("Switched to YOLOV5 model");
            } else if (cmd.find("RETINAFACE") != std::string::npos || cmd.find("retinaface") != std::string::npos) {
                aipc::ai::AIManager::Instance().SwitchModel(aipc::ai::ModelType::RETINAFACE, "./model/retinaface.rknn");
                SPDLOG_INFO("Switched to RETINAFACE model");
            } else if (cmd.find("NONE") != std::string::npos || cmd.find("none") != std::string::npos) {
                aipc::ai::AIManager::Instance().SwitchModel(aipc::ai::ModelType::NONE);
                SPDLOG_INFO("Switched to NONE model");
            } else {
                SPDLOG_WARN("Unknown model in control command: {}", cmd);
            }

            nlohmann::json response;
            response["type"] = "ack";
            response["message"] = "Command processed";
            return response.dump();
        }
        else {
            SPDLOG_WARN("Unknown command type: '{}'", msg.type);
            nlohmann::json error_response;
            error_response["type"] = "error";
            error_response["message"] = std::string("Unknown command type: ") + msg.type;
            return error_response.dump();
        }
    });
    command_listener.start();

    const int width = cfg.width;
    const int height = cfg.height;

    // ISP Init (RAII)
    aipc::rkmpi::IspSession isp(0, cfg.hdr_mode, cfg.multi_sensor, cfg.iq_dir);

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
    aipc::rtsp::RtspStreamer rtsp_streamer(cfg.rtsp_port, cfg.rtsp_path);
    if (!rtsp_streamer.ok() || !rtsp_streamer.startH264()) {
        SPDLOG_ERROR("RTSP init failed");
        return -1;
    }

    // VI/VENC Init (RAII)
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
                // Send to RTSP
                rtsp_streamer.pushH264((uint8_t *) pData, stFrame.pstPack->u32Len, stFrame.pstPack->u64PTS);
                rtsp_streamer.poll();

                // Send to WebRTC if connected
                if (webrtc_streamer.isConnected()) {
                    webrtc_streamer.pushFrame((const uint8_t *) pData, stFrame.pstPack->u32Len, stFrame.pstPack->u64PTS / 1000);  // Convert to ms
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
