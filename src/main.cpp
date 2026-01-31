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

    // 配置流输出
    StreamOutputConfig streamConfig;
    streamConfig.enableRtsp = true;
    streamConfig.rtspConfig.port = 554;
    streamConfig.rtspConfig.path = "/live/0";
    
    // 初始化流管理器（注册消费者，需要在 rkvideo_init 之前）
    if (!stream_manager_init(streamConfig)) {
        LOG_ERROR("Failed to initialize stream manager!");
        return -1;
    }

    // 初始化 rkvideo 模块
    if (rkvideo_init() != 0) {
        LOG_ERROR("Failed to initialize rkvideo module!");
        stream_manager_deinit();
        return -1;
    }

    // 启动流分发
    stream_manager_start();

    LOG_INFO("AIPC initialized successfully");
    LOG_INFO("RTSP stream available at: rtsp://<device_ip>:554/live/0");
    LOG_INFO("Press Ctrl+C to stop...");

    // 主循环
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 清理资源
    LOG_INFO("Shutting down AIPC...");
    
    // 停止流分发
    stream_manager_stop();
    
    // 反初始化
    rkvideo_deinit();
    stream_manager_deinit();
    
    LOG_INFO("=== AIPC Application Terminated ===");
    LogManager::Shutdown();
    
    return 0;
}