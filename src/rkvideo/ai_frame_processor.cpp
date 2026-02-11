/**
 * @file ai_frame_processor.cpp
 * @brief AI 帧处理器实现
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#define LOG_TAG "AIFrameProc"

#include "ai_frame_processor.h"
#include "rknn/ai_engine.h"
#include "common/logger.h"

// OpenCV-mobile
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <chrono>
#include <cstring>
#include <algorithm>

namespace rkvideo {

// ============================================================================
// AIFrameProcessor 实现
// ============================================================================

AIFrameProcessor::AIFrameProcessor() = default;

AIFrameProcessor::~AIFrameProcessor() {
    Deinit();
}

bool AIFrameProcessor::Init(const AIFrameProcessorConfig& config) {
    if (initialized_.load()) {
        Deinit();
    }
    
    config_ = config;
    
    // 初始化图像处理器（用于模型输入准备）
    if (!image_processor_.Init(config_.model_width, config_.model_height)) {
        LOG_ERROR("Failed to initialize ImageProcessor");
        return false;
    }
    
    // 预分配模型输入缓冲区
    model_input_buffer_.resize(config_.model_width * config_.model_height * 3);
    
    // 重置统计
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_ = Stats{};
        total_inference_time_ = 0.0;
        inference_count_ = 0;
    }
    
    frame_counter_.store(0);
    skip_counter_ = 0;
    
    initialized_.store(true);
    
    LOG_INFO("AIFrameProcessor initialized (model_size={}x{}, skip_frames={})", 
             config_.model_width, config_.model_height, config_.skip_frames);
    
    return true;
}

void AIFrameProcessor::Deinit() {
    if (!initialized_.load()) {
        return;
    }
    
    initialized_.store(false);
    
    image_processor_.Deinit();
    model_input_buffer_.clear();
    temp_rgb_buffer_.clear();
    
    LOG_INFO("AIFrameProcessor deinitialized");
}

bool AIFrameProcessor::ProcessFrame(const RawFrame& raw_frame, ProcessedFrame& processed_frame) {
    if (!initialized_.load()) {
        return false;
    }
    
    // 检查 AIEngine 是否有模型
    if (!rknn::AIEngine::Instance().HasModel()) {
        // 没有模型时跳过，但仍需要转换基本的 NV12 → RGB
        // 这样视频流仍可正常编码
        return ConvertNV12ToRGB(raw_frame.data, raw_frame.width, raw_frame.height,
                                raw_frame.stride, processed_frame.data);
    }
    
    uint64_t frame_num = frame_counter_.fetch_add(1);
    
    // 跳帧逻辑
    bool should_infer = true;
    if (config_.skip_frames > 0) {
        skip_counter_++;
        if (skip_counter_ <= static_cast<uint64_t>(config_.skip_frames)) {
            should_infer = false;
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.frames_skipped++;
        } else {
            skip_counter_ = 0;
        }
    }
    
    // ========================================
    // 步骤 1: NV12 → RGB 全分辨率（用于 VENC 编码）
    // ========================================
    if (!ConvertNV12ToRGB(raw_frame.data, raw_frame.width, raw_frame.height,
                          raw_frame.stride, processed_frame.data)) {
        LOG_WARN("Failed to convert NV12 to RGB");
        return false;
    }
    
    if (!should_infer) {
        // 跳帧模式，仅返回转换后的 RGB
        // 但仍然需要更新处理计数
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.frames_processed++;
        return true;
    }
    
    // ========================================
    // 步骤 2: 准备模型输入（缩放 + letterbox）
    // ========================================
    if (!PrepareModelInput(processed_frame.data, raw_frame.width, raw_frame.height)) {
        LOG_WARN("Failed to prepare model input");
        // 仍然返回 true，因为 RGB 已就绪
        return true;
    }
    
    // ========================================
    // 步骤 3: 运行 AI 推理
    // ========================================
    rknn::DetectionResultList results;
    bool inference_ok = RunInference(results);
    
    // ========================================
    // 步骤 4: 绘制检测框（可选）
    // ========================================
    if (inference_ok && config_.draw_boxes && results.Count() > 0) {
        DrawDetections(processed_frame.data, processed_frame.width, processed_frame.height, results);
    }
    
    // 更新统计
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.frames_processed++;
        if (inference_ok) {
            stats_.total_detections += results.Count();
        }
    }
    
    // 日志输出
    if (config_.enable_log && inference_ok) {
        if (results.Count() > 0) {
            LOG_INFO("Frame #{}: {} detections", frame_num, results.Count());
        } else if (frame_num % 30 == 0) {
            LOG_DEBUG("Frame #{}: 0 detections", frame_num);
        }
    }
    
    return true;
}

FrameProcessCallback AIFrameProcessor::GetCallback() {
    return [this](const RawFrame& raw, ProcessedFrame& processed) {
        return this->ProcessFrame(raw, processed);
    };
}

AIFrameProcessor::Stats AIFrameProcessor::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

// ============================================================================
// 内部实现
// ============================================================================

bool AIFrameProcessor::ConvertNV12ToRGB(const void* nv12_data, int width, int height, 
                                         int stride, void* rgb_output) {
    if (!nv12_data || !rgb_output || width <= 0 || height <= 0) {
        return false;
    }
    
    // 如果未指定 stride，使用 width
    if (stride <= 0) {
        stride = width;
    }
    
    // 创建 NV12 Mat
    cv::Mat yuv420sp;
    if (stride == width) {
        // stride 等于 width，直接使用
        yuv420sp = cv::Mat(height + height / 2, width, CV_8UC1, 
                           const_cast<void*>(nv12_data));
    } else {
        // stride 不等于 width，需要逐行拷贝
        yuv420sp = cv::Mat(height + height / 2, width, CV_8UC1);
        
        const uint8_t* src_ptr = static_cast<const uint8_t*>(nv12_data);
        uint8_t* dst_ptr = yuv420sp.data;
        
        // 拷贝 Y 平面
        for (int y = 0; y < height; ++y) {
            std::memcpy(dst_ptr + y * width, src_ptr + y * stride, width);
        }
        
        // 拷贝 UV 平面
        const uint8_t* uv_src = src_ptr + stride * height;
        uint8_t* uv_dst = dst_ptr + width * height;
        for (int y = 0; y < height / 2; ++y) {
            std::memcpy(uv_dst + y * width, uv_src + y * stride, width);
        }
    }
    
    // NV12 -> BGR -> RGB
    cv::Mat bgr, rgb;
    cv::cvtColor(yuv420sp, bgr, cv::COLOR_YUV420sp2BGR);
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    
    // 复制到输出缓冲区
    std::memcpy(rgb_output, rgb.data, width * height * 3);
    
    return true;
}

bool AIFrameProcessor::PrepareModelInput(const void* rgb_data, int width, int height) {
    if (!rgb_data || width <= 0 || height <= 0) {
        return false;
    }
    
    // 计算 letterbox 参数
    float scale_x = static_cast<float>(config_.model_width) / width;
    float scale_y = static_cast<float>(config_.model_height) / height;
    float scale = std::min(scale_x, scale_y);
    
    int scaled_width = static_cast<int>(width * scale);
    int scaled_height = static_cast<int>(height * scale);
    
    int pad_left = (config_.model_width - scaled_width) / 2;
    int pad_top = (config_.model_height - scaled_height) / 2;
    
    // 填充 letterbox 信息（用于坐标映射）
    letterbox_info_.scale = scale;
    letterbox_info_.pad_left = pad_left;
    letterbox_info_.pad_top = pad_top;
    letterbox_info_.src_width = width;
    letterbox_info_.src_height = height;
    letterbox_info_.dst_width = config_.model_width;
    letterbox_info_.dst_height = config_.model_height;
    
    // 创建输入 Mat（不复制数据）
    cv::Mat rgb(height, width, CV_8UC3, const_cast<void*>(rgb_data));
    
    // 缩放
    cv::Mat scaled;
    cv::resize(rgb, scaled, cv::Size(scaled_width, scaled_height), 0, 0, cv::INTER_LINEAR);
    
    // 创建 letterbox 并复制到中心
    cv::Mat letterbox_img(config_.model_height, config_.model_width, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Rect roi(pad_left, pad_top, scaled_width, scaled_height);
    scaled.copyTo(letterbox_img(roi));
    
    // 复制到模型输入缓冲区
    std::memcpy(model_input_buffer_.data(), letterbox_img.data, 
                config_.model_width * config_.model_height * 3);
    
    return true;
}

bool AIFrameProcessor::RunInference(rknn::DetectionResultList& results) {
    auto start = std::chrono::steady_clock::now();
    
    int ret = rknn::AIEngine::Instance().ProcessFrame(
        model_input_buffer_.data(),
        config_.model_width,
        config_.model_height,
        results);
    
    auto end = std::chrono::steady_clock::now();
    double inference_ms = std::chrono::duration<double, std::milli>(end - start).count();
    
    // 更新统计
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        total_inference_time_ += inference_ms;
        inference_count_++;
        stats_.avg_inference_ms = total_inference_time_ / inference_count_;
    }
    
    if (ret != 0) {
        LOG_WARN("Inference failed: ret={}", ret);
        return false;
    }
    
    return true;
}

void AIFrameProcessor::DrawDetections(void* rgb_data, int width, int height,
                                       const rknn::DetectionResultList& results) {
    // 委托给 ImageProcessor 进行绘制
    image_processor_.DrawDetections(rgb_data, width, height, results, letterbox_info_);
}

// ============================================================================
// 便捷工厂函数
// ============================================================================

FrameProcessCallback CreateAIFrameProcessCallback(int model_width, int model_height) {
    // 使用静态实例，生命周期跟随程序
    static std::unique_ptr<AIFrameProcessor> s_processor;
    
    // 如果已存在，先释放
    if (s_processor) {
        s_processor->Deinit();
        s_processor.reset();
    }
    
    // 创建新实例
    s_processor = std::make_unique<AIFrameProcessor>();
    
    AIFrameProcessorConfig config;
    config.model_width = model_width;
    config.model_height = model_height;
    config.skip_frames = 2;    // 每 3 帧推理 1 次
    config.draw_boxes = true;  // 绘制检测框
    config.enable_log = false; // 默认关闭日志
    
    if (!s_processor->Init(config)) {
        LOG_ERROR("Failed to initialize AIFrameProcessor");
        s_processor.reset();
        return nullptr;
    }
    
    LOG_INFO("Created AI frame process callback ({}x{})", model_width, model_height);
    
    return s_processor->GetCallback();
}

}  // namespace rkvideo
