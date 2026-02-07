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

#include <chrono>
#include <cstring>

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
        void* nv12_data = get_frame_vir_addr(frame);
        uint64_t timestamp = frame->stVFrame.u64PTS;
        
        if (!nv12_data) {
            LOG_WARN("Failed to get frame virtual address");
            continue;
        }
        
        // NV12 转换为 RGB 并做 letterbox
        LetterboxInfo letterbox_info;
        int ret = processor.ConvertNV12ToModelInput(nv12_data, width, height,
                                                     model_input_buffer.data(), letterbox_info);
        
        // 数据已拷贝到 model_input_buffer，立即释放 VPSS 帧
        // 归还 DMA buffer 给 VPSS 池，避免长时间占用导致资源冲突
        frame.reset();
        
        if (ret != 0) {
            LOG_WARN("Failed to convert NV12 to RGB: ret={}", ret);
            continue;
        }
        
        // 执行推理
        auto start = std::chrono::steady_clock::now();
        
        DetectionResultList results;
        ret = AIEngine::Instance().ProcessFrame(model_input_buffer.data(),
                                                 processor.GetModelWidth(),
                                                 processor.GetModelHeight(),
                                                 results);
        
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
        
        // 分发检测结果
        if (results.Count() > 0) {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            for (const auto& callback : callbacks_) {
                if (callback) {
                    callback(results, timestamp);
                }
            }
        }
    }
    
    // 清理
    processor.Deinit();
    
    LOG_INFO("Inference loop exited, processed {} frames, avg inference: {:.1f}ms",
             frame_count, inference_count > 0 ? total_inference_time / inference_count : 0);
}

}  // namespace rknn
