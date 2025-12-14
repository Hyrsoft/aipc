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
#include <vector>
#include <thread>
#include <memory>
#include <chrono>

#include <spdlog/spdlog.h>

#include "rkmpi/luckfox_mpi.h"
#include "vi/vi.h"

#include "comm/CommandListener.hpp"
#include "webrtc/SelfTest.hpp"
#include "rtsp/RtspStreamer.hpp"

#include "ai/AIManager.hpp"

#include "osd/Osd.hpp"
#include "utils/Logging.hpp"

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"

#define DISP_WIDTH  720
#define DISP_HEIGHT 480

int width    = DISP_WIDTH;
int height   = DISP_HEIGHT;

int main(int argc, char *argv[]) {
    aipc::logging::Init();

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

    // H264 Frame Setup
    VENC_STREAM_S stFrame;    
    stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
    RK_U64 H264_PTS = 0;
    RK_U32 H264_TimeRef = 0; 
    VIDEO_FRAME_INFO_S stViFrame;
    
    // Create Pool
    MB_POOL_CONFIG_S PoolCfg;
    memset(&PoolCfg, 0, sizeof(MB_POOL_CONFIG_S));
    PoolCfg.u64MBSize = width * height * 3 ;
    PoolCfg.u32MBCnt = 4; // Increase buffer count to avoid tearing/ghosting
    PoolCfg.enAllocType = MB_ALLOC_TYPE_DMA;
    MB_POOL src_Pool = RK_MPI_MB_CreatePool(&PoolCfg);
    SPDLOG_INFO("Create Pool success");

    // Build h264_frame structure (common parts)
    VIDEO_FRAME_INFO_S h264_frame;
    memset(&h264_frame, 0, sizeof(VIDEO_FRAME_INFO_S));
    h264_frame.stVFrame.u32Width = width;
    h264_frame.stVFrame.u32Height = height;
    h264_frame.stVFrame.u32VirWidth = width;
    h264_frame.stVFrame.u32VirHeight = height;
    h264_frame.stVFrame.enPixelFormat =  RK_FMT_RGB888; 
    h264_frame.stVFrame.u32FrameFlag = 160;
    // pMbBlk will be set in the loop

    // ISP Init
    RK_BOOL multi_sensor = RK_FALSE;    
    const char *iq_dir = "/etc/iqfiles";
    rk_aiq_working_mode_t hdr_mode = RK_AIQ_WORKING_MODE_NORMAL;
    SAMPLE_COMM_ISP_Init(0, hdr_mode, multi_sensor, iq_dir);
    SAMPLE_COMM_ISP_Run(0);

    // MPI Init
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        SPDLOG_ERROR("rk mpi sys init fail!");
        return -1;
    }

    // RTSP Init
    aipc::rtsp::RtspStreamer rtsp_streamer(554, "/live/0");
    if (!rtsp_streamer.ok() || !rtsp_streamer.startH264()) {
        SPDLOG_ERROR("RTSP init failed");
        return -1;
    }
    
    // VI Init
    vi_dev_init();
    vi_chn_init(0, width, height);

    // VENC Init
    RK_CODEC_ID_E enCodecType = RK_VIDEO_ID_AVC;
    venc_init(0, width, height, enCodecType);

    SPDLOG_INFO("venc init success");
    
    std::vector<aipc::ai::ObjectDet> results;

    while(1)
    {    
        // Get MB from Pool for this frame
        MB_BLK src_Blk = RK_MPI_MB_GetMB(src_Pool, width * height * 3, RK_TRUE);
        unsigned char *data = (unsigned char *)RK_MPI_MB_Handle2VirAddr(src_Blk);
        h264_frame.stVFrame.pMbBlk = src_Blk;

        // Get VI Frame
        h264_frame.stVFrame.u32TimeRef = H264_TimeRef++;
        h264_frame.stVFrame.u64PTS = TEST_COMM_GetNowUs(); 
        s32Ret = RK_MPI_VI_GetChnFrame(0, 0, &stViFrame, -1);
        if(s32Ret == RK_SUCCESS)
        {
            void *vi_data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);    

            cv::Mat yuv420sp(height + height / 2, width, CV_8UC1, vi_data);
            cv::Mat bgr(height, width, CV_8UC3, data);            
            
            cv::cvtColor(yuv420sp, bgr, cv::COLOR_YUV420sp2BGR);
            // cv::resize(bgr, frame, cv::Size(width ,height), 0, 0, cv::INTER_LINEAR); // Already same size
            
            // Inference
            aipc::ai::AIManager::Instance().RunInference(bgr, results);

            // Draw Results
            aipc::osd::DrawDetections(bgr, results);
        }
        
        // Encode H264
        RK_MPI_VENC_SendFrame(0, &h264_frame, -1);

        // Release MB (VENC should have taken a reference if needed, or we wait for it to finish? 
        // In standard MPI, SendFrame is async but takes reference. We release OUR reference.)
        RK_MPI_MB_ReleaseMB(src_Blk);

        // RTSP Send
        s32Ret = RK_MPI_VENC_GetStream(0, &stFrame, -1);
        if(s32Ret == RK_SUCCESS)
        {
            void *pData = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
            rtsp_streamer.pushH264((uint8_t *)pData, stFrame.pstPack->u32Len, stFrame.pstPack->u64PTS);
            rtsp_streamer.poll();
        }

        // Release Frame
        s32Ret = RK_MPI_VI_ReleaseChnFrame(0, 0, &stViFrame);
        if (s32Ret != RK_SUCCESS) {
            SPDLOG_WARN("RK_MPI_VI_ReleaseChnFrame fail {:x}", static_cast<unsigned int>(s32Ret));
        }
        s32Ret = RK_MPI_VENC_ReleaseStream(0, &stFrame);
        if (s32Ret != RK_SUCCESS) {
            SPDLOG_WARN("RK_MPI_VENC_ReleaseStream fail {:x}", static_cast<unsigned int>(s32Ret));
        }
    }

    // Cleanup
    // RK_MPI_MB_ReleaseMB(src_Blk); // Removed as we release in loop
    RK_MPI_MB_DestroyPool(src_Pool);
    
    RK_MPI_VI_DisableChn(0, 0);
    RK_MPI_VI_DisableDev(0);

    SAMPLE_COMM_ISP_Stop(0);
    
    RK_MPI_VENC_StopRecvFrame(0);
    RK_MPI_VENC_DestroyChn(0);

    free(stFrame.pstPack);
    
    RK_MPI_SYS_Exit();

    return 0;
}
