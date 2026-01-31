// 定义模块名，必须在 #include "logger.h" 之前
#define LOG_TAG "main"

#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include "common/logger.h"
#include "rkvideo/rkvideo.h"
#include "thread_stream.h"

// 全局退出标志
static std::atomic<bool> g_running{true};

// 信号处理函数
static void signal_handler(int sig) {
    LOG_INFO("Received signal {}, shutting down...", sig);
    g_running = false;
}

int main() {
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
    
    // 文件保存配置（按需启用）
    stream_config.enable_file = false;  // 设为 true 启用
    // stream_config.mp4_config.outputDir = "/sdcard/videos";
    // stream_config.jpeg_config.outputDir = "/sdcard/photos";
    // stream_config.jpeg_config.viDevId = 0;
    // stream_config.jpeg_config.viChnId = 0;

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

    LOG_INFO("AIPC initialized successfully");
    LOG_INFO("RTSP stream available at: rtsp://<device_ip>:554/live/0");
    LOG_INFO("Press Ctrl+C to stop...");

    // ========================================================================
    // 主循环 - 这里可以添加控制逻辑
    // 例如：WebSocket 服务、HTTP API、定时任务等
    // ========================================================================
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // TODO: 这里可以添加控制逻辑
        // 例如检查 WebSocket 命令、定时拍照、自动录制等
        //
        // auto* file_thread = GetStreamManager()->GetFileThread();
        // if (file_thread) {
        //     file_thread->StartRecording();   // 开始录制
        //     file_thread->TakeSnapshot();     // 拍照
        //     file_thread->StopRecording();    // 停止录制
        // }
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