// 定义模块名，必须在 #include "logger.h" 之前
#define LOG_TAG "main"

#include <iostream>
#include "common/logger.h"
#include "rkvideo/rkvideo.h"


int main() {
    // 初始化日志系统
    LogManager::init();
    
    LOG_INFO("=== AIPC Application Starting ===");
    
    // 调用SDK提供的脚本，关闭默认启动的rkipc
    LOG_DEBUG("Stopping default rkipc service...");
    system("/oem/usr/bin/RkLunch-stop.sh");
    LOG_INFO("rkipc service stopped");

    // 初始化 rkvideo 模块
    if (rkvideo_init() != 0) {
        LOG_ERROR("Failed to initialize rkvideo module!");
        return -1;
    }

    LOG_INFO("AIPC initialized successfully, running...");

    // TODO: 主循环

    // 清理资源
    LOG_INFO("Shutting down AIPC...");
    rkvideo_deinit();
    
    LOG_INFO("=== AIPC Application Terminated ===");
    LogManager::shutdown();
    
    return 0;
}