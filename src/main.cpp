/**
 * @file main.cpp
 * @brief AIPC 主程序 - 基于 RV1106 的边缘 AI 相机
 *
 * 支持的输出方式：
 * - RTSP: rtsp://<device_ip>:554/live/0
 * - WebRTC: http://<device_ip>:8080 (需要信令服务器)
 * - 文件录制: /root/record/
 *
 * HTTP API: 见 http.h
 * 
 * 线程模型（针对单核 CPU 优化）：
 * - Main IO Thread: asio 事件循环，处理网络发送
 * - Video Fetch Thread: 从 VENC 获取编码流
 * - File I/O Thread: 文件写入（如果启用录制）
 * - HTTP Thread: HTTP API 服务（cpp-httplib）
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
#include <unistd.h>
#include <linux/limits.h>
#include "common/logger.h"
#include "common/asio_context.h"
#include "rkvideo/rkvideo.h"
#include "rkvideo/pipeline_manager.h"
#include "rknn/ai_engine.h"
#include "rknn/ai_service.h"
#include "stream_manager.h"
#include "http.h"

// 全局退出标志
static std::atomic<bool> g_running{true};

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
static void print_startup_info(const StreamConfig& config, int http_port) {
    LOG_INFO("===========================================");
    LOG_INFO("       AIPC - Edge AI Camera");
    LOG_INFO("===========================================");
    LOG_INFO("");
    
    LOG_INFO("HTTP API Server:");
    LOG_INFO("  URL: http://<device_ip>:{}", http_port);
    LOG_INFO("  Web UI: http://<device_ip>:{}/", http_port);
    
    if (config.enable_rtsp) {
        LOG_INFO("RTSP Stream:");
        LOG_INFO("  URL: rtsp://<device_ip>:{}{}", 
                 config.rtsp_config.port, config.rtsp_config.path);
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

// 打印 API 帮助信息
static void print_api_info() {
    LOG_INFO("All services running. Use HTTP API to control or press Ctrl+C to stop.");
    LOG_INFO("  GET  /api/status         - Get system status");
    LOG_INFO("  GET  /api/rtsp/status    - Get RTSP status");
    LOG_INFO("  POST /api/webrtc/start   - Start WebRTC");
    LOG_INFO("  POST /api/webrtc/stop    - Stop WebRTC");
    LOG_INFO("  GET  /api/record/status  - Get recording status");
    LOG_INFO("  POST /api/record/start   - Start recording");
    LOG_INFO("  POST /api/record/stop    - Stop recording");
    LOG_INFO("  GET  /api/ai/status      - Get AI model status");
    LOG_INFO("  POST /api/ai/switch      - Switch AI model");
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
    
    // 文件保存配置 - 默认启用
    stream_config.enable_file = true;
    stream_config.mp4_config.outputDir = "/root/record";
    
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
    // www 目录与 bin 目录同级，所以使用 ../www
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
        } else if (arg == "--no-rtsp") {
            stream_config.enable_rtsp = false;
            LOG_INFO("RTSP disabled via command line");
        } else if (arg == "--no-webrtc") {
            stream_config.enable_webrtc = false;
            LOG_INFO("WebRTC disabled via command line");
        } else if (arg == "--no-ws-preview") {
            stream_config.enable_ws_preview = false;
            LOG_INFO("WebSocket preview disabled via command line");
        } else if (arg == "--help" || arg == "-h") {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --record, -r      Enable file recording\n");
            printf("  --no-rtsp         Disable RTSP streaming\n");
            printf("  --no-webrtc       Disable WebRTC streaming\n");
            printf("  --no-ws-preview   Disable WebSocket preview\n");
            printf("  --help, -h        Show this help\n");
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
    // 初始化 PipelineManager（代替直接调用 rkvideo_init）
    // ========================================================================
    rkvideo::PipelineManagerConfig pipeline_config;
    pipeline_config.ApplyResolution(rkvideo::ResolutionPreset::R_1080P);
    pipeline_config.initial_mode = rkvideo::PipelineMode::PureIPC;  // 默认并行模式
    pipeline_config.ai_width = 640;
    pipeline_config.ai_height = 640;
    
    LOG_INFO("Initializing PipelineManager in PureIPC mode...");
    if (rkvideo::PipelineManager::Instance().Init(pipeline_config) != 0) {
        LOG_ERROR("Failed to initialize PipelineManager!");
        g_http_api->Stop();
        DestroyStreamManager();
        return -1;
    }

    // ========================================================================
    // 初始化 AI 推理引擎（默认不加载模型）
    // ========================================================================
    std::string model_dir = exe_dir + "/../model";
    LOG_INFO("AI model directory: {}", model_dir);
    rknn::AIEngine::Instance().SetModelDir(model_dir);
    
    // 设置 VPSS 重配置回调：当模型输入尺寸变化时，自动重配置 VPSS Chn1
    // 注：仅在并行模式下有效，串行模式不需要这个回调
    rknn::AIEngine::Instance().SetVpssReconfigureCallback(
        [](int width, int height) -> int {
            auto& mgr = rkvideo::PipelineManager::Instance();
            if (mgr.GetCurrentMode() == rkvideo::PipelineMode::PureIPC) {
                LOG_INFO("VPSS reconfigure callback: {}x{}", width, height);
                return rkvideo_reconfigure_ai_channel(width, height);
            }
            // 串行模式下不需要重配置
            return 0;
        }
    );
    
    LOG_INFO("AI Engine initialized (no model loaded by default)");

    // 启动 AI 推理服务
    rknn::AIServiceConfig ai_config;
    ai_config.skip_frames = 2;   // 每 3 帧推理一次（减少 CPU 占用）
    ai_config.timeout_ms = 100;
    ai_config.enable_log = true; // 启用推理日志
    
    // 注册检测结果回调（目前只打印日志，后续可扩展为 OSD 叠加）
    rknn::AIService::Instance().RegisterCallback(
        [](const rknn::DetectionResultList& results, uint64_t timestamp) {
            // TODO: 在视频帧上绘制检测框（需要 RGA 或软件绘制）
            LOG_DEBUG("Detection callback: {} results at timestamp {}",
                     results.Count(), timestamp);
        }
    );
    
    rknn::AIService::Instance().Start(ai_config);

    // 启动流输出
    GetStreamManager()->Start();

    // 打印启动信息
    print_startup_info(stream_config, http_config.port);
    print_api_info();

    // ========================================================================
    // 主事件循环 - 使用 asio 替代 sleep 循环
    // ========================================================================
    LOG_INFO("Starting main IO event loop (optimized for single-core CPU)...");
    
    // 在主线程运行 asio 事件循环
    // 所有网络消费者（RTSP、WebRTC、WebSocket）的发送操作都会投递到这里执行
    // 这样可以消除多线程竞争，减少上下文切换
    IoContext::Instance().Run();
    
    // 事件循环退出后（收到信号）开始清理

    // ========================================================================
    // 清理资源
    // ========================================================================
    LOG_INFO("Shutting down AIPC...");
    
    // 停止 AI 推理服务
    rknn::AIService::Instance().Stop();
    LOG_INFO("AI Service stopped");
    
    // 停止 AI 推理引擎（卸载模型）
    rknn::AIEngine::Instance().SwitchModel(rknn::ModelType::kNone);
    LOG_INFO("AI Engine stopped");
    
    // 停止 HTTP API
    if (g_http_api) {
        g_http_api->Stop();
        g_http_api.reset();
    }
    
    // 停止并销毁流管理器（这会停止 StreamDispatcher）
    DestroyStreamManager();
    
    // 排空 IO Context 中的待处理任务
    // 关键：StreamDispatcher 通过 PostToIo 分发帧给网络消费者（RTSP/WebSocket/WebRTC），
    // io_context.stop() 后这些异步回调持有 EncodedStreamPtr（VENC Buffer 的 shared_ptr），
    // 如果不执行它们，VENC Buffer 不会被 ReleaseStream，导致 VENC DestroyChn 时
    // 死循环打印 "wait for release buffer"。
    // Drain() 会重新 poll 一遍 io_context，让所有 pending 的回调执行完毕并释放资源。
    LOG_DEBUG("Draining IO context to release pending VENC buffers...");
    IoContext::Instance().Drain();
    
    // 反初始化 PipelineManager
    rkvideo::PipelineManager::Instance().Deinit();
    
    LOG_INFO("=== AIPC Application Terminated ===");
    LogManager::Shutdown();
    
    return 0;
}