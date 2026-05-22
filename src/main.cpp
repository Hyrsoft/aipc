/**
 * @file main.cpp
 * @brief AIPC 主程序 - 基于 RV1106 的边缘 AI 相机
 *
 * 支持的输出方式：
 * - RTSP: rtsp://<device_ip>:554/live/0
 * - WebRTC: http://<device_ip>:8080
 * - 文件录制: AIPC_RECORD_DIR 指定目录，默认 /root/record/
 *
 * HTTP API: 见 http.h
 *
 * 使用新的 Producer-based 架构：
 * - MediaManager: 统一管理视频采集模式（SimpleIPC/VisionG）
 * - StreamManager: 管理流分发（RTSP/WebRTC/WebSocket/File）
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

// 定义模块名，必须在 #include "logger.h" 之前
#define LOG_TAG "main"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <linux/limits.h>
#include <thread>
#include <unistd.h>
#include "common/asio_context.h"
#include "common/logger.h"
#include "http.h"
#include "media_distribution/file/file_service.h"
#include "media_distribution/rtsp/rtsp_service.h"
#include "media_distribution/stream_manager.h"
#include "media_distribution/webrtc/webrtc_service.h"
#include "media_distribution/wspreview/ws_preview.h"
#include "media_producer/media_manager.h"
#include "media_producer/visiong/visiong_producer.h"

// ========================================================================
// 全局配置
// ========================================================================
static std::atomic<bool> g_running{true};

// 默认启动模式（修改这里可以切换初始模式）
static media::ProducerMode g_startup_mode = media::ProducerMode::SimpleIPC;

// 全局 HTTP API 实例
static std::unique_ptr<HttpApi> g_http_api;

// 获取可执行文件所在目录
static std::string get_exe_dir() {
    char path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len != -1) {
        path[len] = '\0';
        std::string exe_path(path);
        size_t pos = exe_path.rfind('/');
        if (pos != std::string::npos) {
            return exe_path.substr(0, pos);
        }
    }
    return ".";
}

// 信号处理函数
static void signal_handler(int sig) {
    LOG_INFO("Received signal {}, shutting down...", sig);
    g_running = false;

    // 停止 asio IO 循环
    IoContext::Instance().Stop();
}

// 打印启动信息
static void print_startup_info(const StreamConfig &config, int http_port) {
    LOG_INFO("===========================================");
    LOG_INFO("       AIPC - Edge AI Camera");
    LOG_INFO("===========================================");
    LOG_INFO("");

    LOG_INFO("HTTP API Server:");
    LOG_INFO("  URL: http://<device_ip>:{}", http_port);
    LOG_INFO("  Web UI: http://<device_ip>:{}/", http_port);

    if (config.enable_rtsp) {
        LOG_INFO("RTSP Stream:");
        LOG_INFO("  URL: rtsp://<device_ip>:{}{}", config.rtsp_config.port, config.rtsp_config.path);
    }

    if (config.enable_webrtc) {
        LOG_INFO("WebRTC Stream:");
        LOG_INFO("  Signaling: {}", config.webrtc_config.signaling_url);
    }

    if (config.enable_ws_preview) {
        LOG_INFO("WebSocket Preview:");
        LOG_INFO("  URL: ws://<device_ip>:{}", config.ws_preview_config.port);
    }

    if (config.enable_file) {
        LOG_INFO("Recording:");
        LOG_INFO("  Output: {}", config.mp4_config.outputDir);
    }

    LOG_INFO("");
    LOG_INFO("Press Ctrl+C to stop...");
    LOG_INFO("===========================================");
}

int main(int argc, char *argv[]) {
    // 初始化日志系统
    LogManager::Init();

    LOG_INFO("=== AIPC Application Starting ===");

    // 在主线程早期预热 Python/visiong，避免首次切换 VisionG 时在工作线程中首次导入。
    media::WarmupVisionGPythonRuntime();

    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 调用 SDK 提供的脚本关闭默认启动的 rkipc。可通过环境变量覆盖，便于不同板端布局。
    const char *rkipc_stop_cmd = std::getenv("AIPC_RKIPC_STOP_CMD");
    if (!rkipc_stop_cmd) {
        rkipc_stop_cmd = "/oem/usr/bin/RkLunch-stop.sh";
    }
    if (rkipc_stop_cmd[0] != '\0') {
        LOG_DEBUG("Stopping default rkipc service: {}", rkipc_stop_cmd);
        int stop_ret = system(rkipc_stop_cmd);
        if (stop_ret != 0) {
            LOG_WARN("rkipc stop command returned {}", stop_ret);
        } else {
            LOG_INFO("rkipc service stopped");
        }
    } else {
        LOG_INFO("rkipc stop command disabled by AIPC_RKIPC_STOP_CMD");
    }

    // ========================================================================
    // 配置流输出
    // ========================================================================
    StreamConfig stream_config;

    // RTSP 配置 - 创建服务但不自动启动，通过 WebUI 控制
    stream_config.enable_rtsp = true;
    stream_config.auto_start_rtsp = false; // 不自动启动
    stream_config.rtsp_config.port = 554;
    stream_config.rtsp_config.path = "/live/0";

    // WebRTC 配置 - 创建服务但不自动启动，通过 WebUI 控制
    stream_config.enable_webrtc = true;
    stream_config.auto_start_webrtc = false; // 不自动启动
    stream_config.webrtc_config.device_id = "aipc_camera";

    // 从环境变量获取信令服务器地址，默认为 localhost
    const char *signaling_host = std::getenv("SIGNALING_HOST");
    if (!signaling_host) {
        signaling_host = "127.0.0.1";
    }
    stream_config.webrtc_config.signaling_url = std::string("ws://") + signaling_host + ":8000/";

    // WebRTC 视频参数配置
    stream_config.webrtc_config.webrtc_config.video.width = 1920;
    stream_config.webrtc_config.webrtc_config.video.height = 1080;
    stream_config.webrtc_config.webrtc_config.video.fps = 30;

    // 文件保存配置 - 默认启用
    stream_config.enable_file = true;
    const char *record_dir = std::getenv("AIPC_RECORD_DIR");
    stream_config.mp4_config.outputDir = record_dir ? record_dir : "/root/record";

    // WebSocket 预览配置 - 默认启用
    stream_config.enable_ws_preview = true;
    stream_config.ws_preview_config.port = 8082;

    // ========================================================================
    // HTTP API 配置
    // ========================================================================
    std::string exe_dir = get_exe_dir();
    LOG_DEBUG("Executable directory: {}", exe_dir);

    HttpApiConfig http_config;
    http_config.host = "0.0.0.0";
    http_config.port = 8080;
    http_config.static_dir = exe_dir + "/../www";
    http_config.thread_pool_size = 2;

    // ========================================================================
    // 命令行参数解析
    // ========================================================================
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--record" || arg == "-r") {
            stream_config.enable_file = true;
            LOG_INFO("Recording enabled via command line");
        } else if (arg == "--rtsp") {
            stream_config.auto_start_rtsp = true;
            LOG_INFO("RTSP auto-start enabled via command line");
        } else if (arg == "--webrtc") {
            stream_config.auto_start_webrtc = true;
            LOG_INFO("WebRTC auto-start enabled via command line");
        } else if (arg == "--no-ws-preview") {
            stream_config.enable_ws_preview = false;
            LOG_INFO("WebSocket preview disabled via command line");
        } else if (arg == "--mode" && i + 1 < argc) {
            std::string mode_str = argv[++i];
            if (mode_str == "simple_ipc" || mode_str == "ipc") {
                g_startup_mode = media::ProducerMode::SimpleIPC;
            } else if (mode_str == "visiong" || mode_str == "yolov5" || mode_str == "yolo") {
                // 当前 VisionG 模式先实现 YOLOv5
                g_startup_mode = media::ProducerMode::VisionG;
            } else {
                printf("Unknown mode: %s (valid: simple_ipc, visiong)\n", mode_str.c_str());
                return -1;
            }
            LOG_INFO("Startup mode set to: {}", media::ProducerModeToString(g_startup_mode));
        } else if (arg == "--help" || arg == "-h") {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --mode <mode>     Startup mode: simple_ipc|visiong (default: simple_ipc)\n");
            printf("  --record, -r      Enable file recording\n");
            printf("  --rtsp            Auto-start RTSP server on startup\n");
            printf("  --webrtc          Auto-start WebRTC server on startup\n");
            printf("  --no-ws-preview   Disable WebSocket preview\n");
            printf("  --help, -h        Show this help\n");
            printf("\nNotes:\n");
            printf("  RTSP and WebRTC services are created but not started by default.\n");
            printf("  Use --rtsp/--webrtc to auto-start, or control via WebUI.\n");
            printf("\nEnvironment variables:\n");
            printf("  SIGNALING_HOST  WebRTC signaling server host (default: 127.0.0.1)\n");
            return 0;
        }
    }

    // ========================================================================
    // 创建流管理器
    // ========================================================================
    CreateStreamManager(stream_config);

    if (!GetStreamManager()) {
        LOG_ERROR("Failed to create stream manager!");
        return -1;
    }

    // ========================================================================
    // 创建并启动 HTTP API
    // ========================================================================
    g_http_api = std::make_unique<HttpApi>();

    if (!g_http_api->Init(http_config, stream_config)) {
        LOG_ERROR("Failed to initialize HTTP API!");
        DestroyStreamManager();
        return -1;
    }

    if (!g_http_api->Start()) {
        LOG_ERROR("Failed to start HTTP API!");
        DestroyStreamManager();
        return -1;
    }

    // ========================================================================
    // 初始化 MediaManager（新的 Producer-based 架构）
    // ========================================================================
    media::ProducerConfig producer_config;
    producer_config.framerate = 30;
    producer_config.bitrate_kbps = 10 * 1024; // 10 Mbps

    LOG_INFO("Initializing MediaManager in {} mode...", media::ProducerModeToString(g_startup_mode));
    auto &media_manager = media::MediaManager::Instance();

    if (media_manager.Init(g_startup_mode, producer_config) != 0) {
        LOG_ERROR("Failed to initialize MediaManager!");
        g_http_api->Stop();
        DestroyStreamManager();
        return -1;
    }

    // 注册流消费者（连接 Producer 和 StreamManager）
    LOG_INFO("Connecting MediaManager to StreamManager...");

    auto *stream_mgr = GetStreamManager();

    // 注册 RTSP 消费者
    if (stream_mgr->GetRtspService()) {
        media_manager.RegisterStreamConsumer(
                "rtsp",
                [](EncodedStreamPtr stream) {
                    RtspService::StreamConsumer(stream, GetStreamManager()->GetRtspService());
                },
                media::StreamConsumerType::AsyncIO);
        LOG_INFO("RTSP consumer registered");
    }

    // 注册 WebSocket 预览消费者
    if (stream_mgr->GetWsPreviewServer()) {
        media_manager.RegisterStreamConsumer(
                "ws_preview",
                [](EncodedStreamPtr stream) {
                    WsPreviewServer::StreamConsumer(stream, GetStreamManager()->GetWsPreviewServer());
                },
                media::StreamConsumerType::AsyncIO);
        LOG_INFO("WebSocket preview consumer registered");
    }

    // 注册文件保存消费者
    if (stream_mgr->GetFileService()) {
        media_manager.RegisterStreamConsumer(
                "file",
                [](EncodedStreamPtr stream) {
                    FileService::StreamConsumer(stream, GetStreamManager()->GetFileService());
                },
                media::StreamConsumerType::Queued, 10);
        LOG_INFO("File consumer registered");
    }

    // 注册 WebRTC 消费者
    if (stream_mgr->GetWebRTCService()) {
        media_manager.RegisterStreamConsumer(
                "webrtc",
                [](EncodedStreamPtr stream) {
                    WebRTCService::StreamConsumer(stream, GetStreamManager()->GetWebRTCService());
                },
                media::StreamConsumerType::AsyncIO);
        LOG_INFO("WebRTC consumer registered");
    }

    // 启动视频采集
    if (!media_manager.Start()) {
        LOG_ERROR("Failed to start MediaManager!");
        g_http_api->Stop();
        DestroyStreamManager();
        return -1;
    }

    // 启动流输出
    GetStreamManager()->Start();

    // 打印启动信息
    print_startup_info(stream_config, http_config.port);
    LOG_INFO("All services running. Use HTTP API to control or press Ctrl+C to stop.");

    // ========================================================================
    // 主事件循环 - 使用 asio 替代 sleep 循环
    // ========================================================================
    LOG_INFO("Starting main IO event loop (optimized for single-core CPU)...");

    // 在主线程运行 asio 事件循环
    IoContext::Instance().Run();

    // 事件循环退出后（收到信号）开始清理

    // ========================================================================
    // 清理资源
    // ========================================================================
    LOG_INFO("Shutting down AIPC...");

    // 1. 先清除流消费者（防止继续 post 新帧到 IoContext）
    LOG_DEBUG("Clearing stream consumers...");
    media_manager.ClearStreamConsumers();

    // 2. 停止 MediaManager（等待 fetch 线程退出）
    media_manager.Stop();
    LOG_INFO("MediaManager stopped");

    // 3. 排空 IO Context 中的待处理任务（释放已 post 的帧）
    LOG_DEBUG("Draining IO context to release pending buffers...");
    IoContext::Instance().Drain();

    // 4. 现在可以安全地反初始化 MPI（释放 VENC 等硬件资源）
    media_manager.Deinit();
    LOG_INFO("MediaManager deinitialized");

    // 5. 停止 HTTP API
    if (g_http_api) {
        g_http_api->Stop();
        g_http_api.reset();
    }

    // 6. 停止并销毁流管理器
    DestroyStreamManager();

    LOG_INFO("=== AIPC Application Terminated ===");
    LogManager::Shutdown();

    return 0;
}
