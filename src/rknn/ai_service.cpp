/**
 * @file ai_service.cpp
 * @brief AI 推理服务实现
 *
 * @author 好软，好温暖
 * @date 2026-02-05
 */

#define LOG_TAG "AIService"

#include "ai_service.h"
#include "ai_engine.h"
#include "image_utils.h"
#include "common/logger.h"
#include "common/media_buffer.h"
#include "rkvideo/rkvideo.h"
#include "rkvideo/osd_overlay.h"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>

// 诊断模式开关：保存 VPSS 原始帧和模型输入帧到 /root/record/vpss/
// 启用后不执行 NPU 推理，仅保存图像用于排查问题
#define AI_DIAG_SAVE_FRAMES 0

#if AI_DIAG_SAVE_FRAMES
// 查看方式：
//   NV12: ffplay -f rawvideo -pix_fmt nv12 -video_size WxH file.nv12
//   RGB:  ffplay -f rawvideo -pix_fmt rgb24 -video_size WxH file.rgb
#endif

// ============================================================================
// BandwidthGuard RAII - 推理期间自动节流 VENC
// ============================================================================

/**
 * @brief RAII 类，在构造时启用 VENC 节流，析构时恢复正常帧率
 * 
 * 解决 DDR 带宽瓶颈：RV1106 单通道 256MB DDR3 在 VENC+NPU 同时工作时
 * 可能导致 NPU 超时。通过在推理期间降低 VENC 编码帧率来缓解。
 */
class BandwidthGuard {
public:
    explicit BandwidthGuard(bool enable = true) : enabled_(enable) {
        if (enabled_) {
            rkvideo_set_venc_throttle(true);
        }
    }
    
    ~BandwidthGuard() {
        if (enabled_) {
            rkvideo_set_venc_throttle(false);
        }
    }
    
    // 禁止拷贝
    BandwidthGuard(const BandwidthGuard&) = delete;
    BandwidthGuard& operator=(const BandwidthGuard&) = delete;
    
private:
    bool enabled_;
};

namespace rknn {

// ============================================================================
// AIService 实现
// ============================================================================

AIService& AIService::Instance() {
    static AIService instance;
    return instance;
}

AIService::~AIService() {
    Stop();
}

bool AIService::Start(const AIServiceConfig& config) {
    if (running_.load()) {
        LOG_WARN("AI Service already running");
        return true;
    }

    config_ = config;
    
    // 重置统计
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_ = Stats{};
    }
    
    running_.store(true);
    thread_ = std::thread(&AIService::InferenceLoop, this);
    
    LOG_INFO("AI Service started (skip_frames={}, timeout={}ms)",
             config_.skip_frames, config_.timeout_ms);
    return true;
}

void AIService::Stop() {
    if (!running_.load()) {
        return;
    }
    
    running_.store(false);
    
    if (thread_.joinable()) {
        thread_.join();
    }
    
    LOG_INFO("AI Service stopped");
}

void AIService::RegisterCallback(DetectionCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callbacks_.push_back(std::move(callback));
}

void AIService::ClearCallbacks() {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callbacks_.clear();
}

AIService::Stats AIService::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void AIService::InferenceLoop() {
    LOG_INFO("Inference loop started");
    
    // 图像处理器
    ImageProcessor processor;
    bool processor_initialized = false;
    
    // RGB 缓冲区（用于模型输入）
    std::vector<uint8_t> model_input_buffer;
    
    uint64_t frame_count = 0;
    uint64_t skip_counter = 0;
    double total_inference_time = 0.0;
    uint64_t inference_count = 0;

#if AI_DIAG_SAVE_FRAMES
    // ========== 诊断模式 ==========
    // 创建输出目录
    static const char* kDiagDir = "/root/record/vpss";
    mkdir("/root/record", 0755);
    mkdir(kDiagDir, 0755);
    
    int diag_save_count = 0;
    auto last_save_time = std::chrono::steady_clock::now();
    static const int kDiagIntervalMs = 5000;  // 每 5 秒保存一批
    LOG_WARN("=== AI DIAGNOSTIC MODE ENABLED ===");
    LOG_WARN("Saving VPSS raw frames and model input to {}", kDiagDir);
    LOG_WARN("NPU inference is DISABLED in this mode");
#endif
    
    while (running_.load()) {
        // 检查是否有模型加载
        if (!AIEngine::Instance().HasModel()) {
            // 没有模型，休眠一段时间再检查
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            processor_initialized = false;  // 模型变化时需要重新初始化
            continue;
        }
        
        // 初始化图像处理器（需要知道模型输入尺寸）
        if (!processor_initialized) {
            int model_w, model_h;
            if (AIEngine::Instance().GetRequiredInputSize(model_w, model_h)) {
                if (processor.Init(model_w, model_h)) {
                    model_input_buffer.resize(model_w * model_h * 3);
                    processor_initialized = true;
                    LOG_INFO("Image processor initialized for model input {}x{}", model_w, model_h);
                } else {
                    LOG_ERROR("Failed to initialize image processor");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    continue;
                }
            }
        }
        
        // 从 VPSS Chn1 获取原始帧
        auto frame = rkvideo_get_vi_frame(config_.timeout_ms);
        if (!frame) {
            continue;  // 超时或错误，继续尝试
        }
        
        frame_count++;
        
        // 跳帧逻辑
        if (config_.skip_frames > 0) {
            skip_counter++;
            if (skip_counter <= static_cast<uint64_t>(config_.skip_frames)) {
                // 跳过这一帧
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.frames_skipped++;
                continue;
            }
            skip_counter = 0;
        }
        
        // 获取帧信息
        int width = frame->stVFrame.u32Width;
        int height = frame->stVFrame.u32Height;
        int stride = frame->stVFrame.u32VirWidth;  // 虚拟宽度（行步长）
        void* nv12_data = get_frame_vir_addr(frame);
#if !AI_DIAG_SAVE_FRAMES
        uint64_t timestamp = frame->stVFrame.u64PTS;
#endif
        
        if (!nv12_data) {
            LOG_WARN("Failed to get frame virtual address");
            continue;
        }

#if AI_DIAG_SAVE_FRAMES
        // ========== 诊断模式：保存帧 ==========
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_save_time).count();
        
        bool should_save = (elapsed >= kDiagIntervalMs);
        
        // 保存 VPSS 原始 NV12 帧
        if (should_save) {
            int nv12_size = width * (height + height / 2);
            
            char filename[256];
            snprintf(filename, sizeof(filename), "%s/%03d_vpss_raw_%dx%d.nv12",
                     kDiagDir, diag_save_count, width, height);
            FILE* fp = fopen(filename, "wb");
            if (fp) {
                fwrite(nv12_data, 1, nv12_size, fp);
                fclose(fp);
                LOG_INFO("[DIAG] Saved VPSS raw NV12: {} ({}x{}, {} bytes)", filename, width, height, nv12_size);
                LOG_INFO("[DIAG]   ffplay -f rawvideo -pix_fmt nv12 -video_size {}x{} {}", width, height, filename);
            }
        }
        
        // 执行 NV12 -> RGB letterbox 转换（和正常推理一样的处理）
        LetterboxInfo letterbox_info;
        int ret = processor.ConvertNV12ToModelInput(nv12_data, width, height, stride,
                                                     model_input_buffer.data(), letterbox_info);
        
        // 数据已拷贝，释放 VPSS 帧
        frame.reset();
        
        if (ret != 0) {
            LOG_WARN("Failed to convert NV12 to RGB: ret={}", ret);
            continue;
        }
        
        if (should_save) {
            // 保存送给 NPU 的模型输入（RGB24 letterbox）
            int model_w = processor.GetModelWidth();
            int model_h = processor.GetModelHeight();
            int rgb_size = model_w * model_h * 3;
            
            char filename[256];
            snprintf(filename, sizeof(filename), "%s/%03d_model_input_%dx%d.rgb",
                     kDiagDir, diag_save_count, model_w, model_h);
            FILE* fp = fopen(filename, "wb");
            if (fp) {
                fwrite(model_input_buffer.data(), 1, rgb_size, fp);
                fclose(fp);
                LOG_INFO("[DIAG] Saved model input RGB: {} ({}x{}, {} bytes, scale={:.3f}, pad=({},{}))",
                         filename, model_w, model_h, rgb_size,
                         letterbox_info.scale, letterbox_info.pad_left, letterbox_info.pad_top);
                LOG_INFO("[DIAG]   ffplay -f rawvideo -pix_fmt rgb24 -video_size {}x{} {}", model_w, model_h, filename);
            }
            
            diag_save_count++;
            last_save_time = now;
            LOG_INFO("[DIAG] Batch #{} saved. Next save in {}s", diag_save_count, kDiagIntervalMs / 1000);
        }
        
        // 诊断模式下跳过 NPU 推理
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.frames_processed++;
        }
        continue;
        
#else
        // ========== 正常推理模式 ==========
        // NV12 转换为 RGB 并做 letterbox
        LetterboxInfo letterbox_info;
        int ret = processor.ConvertNV12ToModelInput(nv12_data, width, height, stride,
                                                     model_input_buffer.data(), letterbox_info);
        
        // 数据已拷贝到 model_input_buffer，立即释放 VPSS 帧
        // 归还 DMA buffer 给 VPSS 池，避免长时间占用导致资源冲突
        // frame.reset();
        
        if (ret != 0) {
            LOG_WARN("Failed to convert NV12 to RGB: ret={}", ret);
            continue;
        }
        
        // 执行推理（使用 BandwidthGuard RAII 自动节流 VENC）
        // 在推理期间降低 VENC 编码帧率，减少 DDR 带宽占用
        auto start = std::chrono::steady_clock::now();
        
        DetectionResultList results;
        {
            BandwidthGuard guard;  // 构造时启用节流，析构时恢复
            ret = AIEngine::Instance().ProcessFrame(model_input_buffer.data(),
                                                     processor.GetModelWidth(),
                                                     processor.GetModelHeight(),
                                                     results);
        }
        
        auto end = std::chrono::steady_clock::now();
        
        double inference_ms = std::chrono::duration<double, std::milli>(end - start).count();
        total_inference_time += inference_ms;
        inference_count++;
        
        // 更新统计
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.frames_processed++;
            stats_.total_detections += results.Count();
            stats_.avg_inference_ms = total_inference_time / inference_count;
        }
        
        if (ret != 0) {
            LOG_WARN("Inference failed: ret={}", ret);
            // 推理失败后延迟一段时间，避免快速重试导致 NPU 过载
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // 打印推理日志
        if (config_.enable_log) {
            if (results.Count() > 0) {
                LOG_INFO("Frame #{}: {} detections, {:.1f}ms",
                         frame_count, results.Count(), inference_ms);
                
                // 打印每个检测结果（使用模型特定的格式）
                for (size_t i = 0; i < results.Count(); ++i) {
                    std::string log = AIEngine::Instance().FormatResultLog(
                        results.results[i], i,
                        letterbox_info.scale,
                        letterbox_info.pad_left,
                        letterbox_info.pad_top);
                    LOG_DEBUG("  {}", log);
                }
            } else if (frame_count % 30 == 0) {
                // 每 30 帧打印一次无检测日志
                LOG_DEBUG("Frame #{}: 0 detections, {:.1f}ms", frame_count, inference_ms);
            }
        }
        
        // 更新 OSD 显示检测框（委托给模型特化实现）
        {
            std::vector<OSDBox> osd_boxes;
            AIEngine::Instance().GenerateOSDBoxes(results, osd_boxes);
            osd_update_boxes(osd_boxes);
        }
        
        // 分发检测结果
        if (results.Count() > 0) {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            for (const auto& callback : callbacks_) {
                if (callback) {
                    callback(results, timestamp);
                }
            }
        }
#endif
    }
    
    // 清理
    processor.Deinit();
    
    LOG_INFO("Inference loop exited, processed {} frames, avg inference: {:.1f}ms",
             frame_count, inference_count > 0 ? total_inference_time / inference_count : 0);
}

}  // namespace rknn
