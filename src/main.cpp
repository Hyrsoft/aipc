/**
 * @file main.cpp
 * @brief AIPC 主程序 - 基于 RV1106 的边缘 AI 相机
 *
 * 支持的输出方式：
 * - RTSP: rtsp://<device_ip>:554/live/0
 * - WebRTC: http://<device_ip>:8080 (需要信令服务器)
 * - 文件录制: /root/record/
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

// 定义模块名，必须在 #include "logger.h" 之前
#define LOG_TAG "main"

#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>
#include "common/logger.h"
#include "rkvideo/rkvideo.h"
#include "thread_stream.h"
#include "file/thread_file.h"

// 全局退出标志
static std::atomic<bool> g_running{true};

// 信号处理函数
static void signal_handler(int sig) {
    LOG_INFO("Received signal {}, shutting down...", sig);
    g_running = false;
}

// 打印启动信息
static void print_startup_info(const StreamConfig& config) {
    LOG_INFO("===========================================");
    LOG_INFO("       AIPC - Edge AI Camera");
    LOG_INFO("===========================================");
    LOG_INFO("");
    
    if (config.enable_rtsp) {
        LOG_INFO("RTSP Stream:");
        LOG_INFO("  URL: rtsp://<device_ip>:{}{}", 
                 config.rtsp_config.port, config.rtsp_config.path);
    }
    
    if (config.enable_webrtc) {
        LOG_INFO("WebRTC Stream:");
        LOG_INFO("  Signaling: {}", config.webrtc_config.signaling_url);
        LOG_INFO("  Web UI: http://<device_ip>:8080");
    }
    
    if (config.enable_file) {
        LOG_INFO("Recording:");
        LOG_INFO("  Output: {}", config.mp4_config.outputDir);
    }
    
    LOG_INFO("");
    LOG_INFO("Press Ctrl+C to stop...");
    LOG_INFO("===========================================");
}

int main(int argc, char* argv[]) {
    // 初始化日志系统
    LogManager::Init();
    
    LOG_INFO("=== AIPC Application Starting ===");

    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 调用SDK提供的脚本，关闭默认启动的rkipc
    LOG_DEBUG("Stopping default rkipc service...");
    system("/oem/usr/bin/RkLunch-stop.sh");
    LOG_INFO("rkipc service stopped");

    // ========================================================================
    // 配置流输出
    // ========================================================================
    StreamConfig stream_config;
    
    // RTSP 配置
    stream_config.enable_rtsp = true;
    stream_config.rtsp_config.port = 554;
    stream_config.rtsp_config.path = "/live/0";
    
    // WebRTC 配置
    stream_config.enable_webrtc = true;
    stream_config.webrtc_config.device_id = "aipc_camera";
    // 从环境变量获取信令服务器地址，默认为 localhost
    const char* signaling_host = std::getenv("SIGNALING_HOST");
    if (!signaling_host) {
        signaling_host = "127.0.0.1";
    }
    stream_config.webrtc_config.signaling_url = 
        std::string("ws://") + signaling_host + ":8000/";
    
    // WebRTC 视频参数配置
    stream_config.webrtc_config.webrtc_config.video.width = 1920;
    stream_config.webrtc_config.webrtc_config.video.height = 1080;
    stream_config.webrtc_config.webrtc_config.video.fps = 30;
    
    // 文件保存配置 - 默认禁用，可通过命令行参数启用
    stream_config.enable_file = false;
    stream_config.mp4_config.outputDir = "/root/record";
    
    // 简单命令行参数解析
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--record" || arg == "-r") {
            stream_config.enable_file = true;
            LOG_INFO("Recording enabled via command line");
        } else if (arg == "--no-rtsp") {
            stream_config.enable_rtsp = false;
            LOG_INFO("RTSP disabled via command line");
        } else if (arg == "--no-webrtc") {
            stream_config.enable_webrtc = false;
            LOG_INFO("WebRTC disabled via command line");
        } else if (arg == "--help" || arg == "-h") {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --record, -r    Enable file recording\n");
            printf("  --no-rtsp       Disable RTSP streaming\n");
            printf("  --no-webrtc     Disable WebRTC streaming\n");
            printf("  --help, -h      Show this help\n");
            printf("\nEnvironment variables:\n");
            printf("  SIGNALING_HOST  WebRTC signaling server host (default: 127.0.0.1)\n");
            return 0;
        }
    }

    // ========================================================================
    // 创建流管理器（需要在 rkvideo_init 之前，以便注册消费者）
    // ========================================================================
    CreateStreamManager(stream_config);
    
    if (!GetStreamManager()) {
        LOG_ERROR("Failed to create stream manager!");
        return -1;
    }

    // ========================================================================
    // 初始化 rkvideo 模块
    // ========================================================================
    if (rkvideo_init() != 0) {
        LOG_ERROR("Failed to initialize rkvideo module!");
        DestroyStreamManager();
        return -1;
    }

    // 启动流输出
    GetStreamManager()->Start();

    // 打印启动信息
    print_startup_info(stream_config);

    // ========================================================================
    // 主循环
    // ========================================================================
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ========================================================================
    // 清理资源
    // ========================================================================
    LOG_INFO("Shutting down AIPC...");
    
    // 停止并销毁流管理器
    DestroyStreamManager();
    
    // 反初始化 rkvideo
    rkvideo_deinit();
    
    LOG_INFO("=== AIPC Application Terminated ===");
    LogManager::Shutdown();
    
    return 0;
}