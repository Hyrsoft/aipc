/**
 * @file serial_frame_processor.cpp
 * @brief 串行模式帧处理器实现
 *
 * @author 好软，好温暖
 * @date 2026-02-11
 */

#define LOG_TAG "SerialProc"

#include "serial_frame_processor.h"
#include "ai_engine.h"
#include "common/logger.h"

// OpenCV 用于 NV12 -> RGB 转换
#include "opencv2/core/core.hpp"
#include "opencv2/imgproc/imgproc.hpp"

#include <chrono>

namespace rknn {

// ============================================================================
// 单例实现
// ============================================================================

SerialFrameProcessor& SerialFrameProcessor::Instance() {
    static SerialFrameProcessor instance;
    return instance;
}

// ============================================================================
// 初始化
// ============================================================================

bool SerialFrameProcessor::Init(const SerialProcessorConfig& config) {
    if (initialized_) {
        LOG_WARN("Serial frame processor already initialized");
        return true;
    }
    
    config_ = config;
    initialized_ = true;
    
    LOG_INFO("Serial frame processor initialized: inference={}, osd={}, skip_frames={}",
             config_.enable_inference, config_.enable_osd, config_.skip_frames);
    return true;
}

void SerialFrameProcessor::SetConfig(const SerialProcessorConfig& config) {
    config_ = config;
    skip_counter_ = 0;  // 重置跳帧计数
    LOG_DEBUG("Config updated: inference={}, osd={}, skip_frames={}",
              config_.enable_inference, config_.enable_osd, config_.skip_frames);
}

// ============================================================================
// 获取处理回调
// ============================================================================

rkvideo::FrameProcessCallback SerialFrameProcessor::GetProcessCallback() {
    return [this](const rkvideo::RawFrame& raw, rkvideo::ProcessedFrame& processed) {
        return this->ProcessFrame(raw, processed);
    };
}

// ============================================================================
// 核心帧处理
// ============================================================================

bool SerialFrameProcessor::ProcessFrame(const rkvideo::RawFrame& raw, 
                                          rkvideo::ProcessedFrame& processed) {
    if (!initialized_) {
        LOG_ERROR("Processor not initialized");
        return false;
    }
    
    auto start_time = std::chrono::steady_clock::now();
    
    frame_count_++;
    
    // 1. NV12 -> RGB 转换
    ConvertNV12ToRGB(raw, processed.data);
    
    // 2. AI 推理（根据配置和跳帧逻辑）
    std::vector<DetectionResult> results;
    bool do_inference = config_.enable_inference && AIEngine::Instance().HasModel();
    
    if (do_inference) {
        // 跳帧逻辑
        if (config_.skip_frames > 0) {
            skip_counter_++;
            if (skip_counter_ <= static_cast<uint64_t>(config_.skip_frames)) {
                // 使用上一帧的结果
                std::lock_guard<std::mutex> lock(results_mutex_);
                results = latest_results_;
                do_inference = false;
            } else {
                skip_counter_ = 0;
            }
        }
    }
    
    if (do_inference) {
        // 执行推理
        DetectionResultList result_list;
        int ret = AIEngine::Instance().ProcessFrame(processed.data, 
                                                     processed.width, 
                                                     processed.height, 
                                                     result_list);
        if (ret == 0) {
            results = result_list.results;
            
            // 更新最新结果
            {
                std::lock_guard<std::mutex> lock(results_mutex_);
                latest_results_ = results;
            }
            
            // 更新统计
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.inferences_done++;
            }
        }
    } else if (!config_.enable_inference) {
        // 推理禁用时，清空结果
        std::lock_guard<std::mutex> lock(results_mutex_);
        latest_results_.clear();
    }
    
    // 3. OSD 叠加
    if (config_.enable_osd && !results.empty()) {
        DrawDetections(processed.data, processed.width, processed.height, results);
    }
    
    // 设置时间戳
    processed.pts = raw.pts;
    
    // 更新统计
    auto end_time = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.frames_processed++;
        total_time_ += elapsed_ms;
        stats_.avg_process_time_ms = total_time_ / stats_.frames_processed;
    }
    
    return true;
}

// ============================================================================
// NV12 -> RGB 转换
// ============================================================================

void SerialFrameProcessor::ConvertNV12ToRGB(const rkvideo::RawFrame& raw, void* rgb_out) {
    // 使用 OpenCV 进行颜色空间转换
    // NV12 格式：Y 平面 + UV 交织平面，总大小为 width * height * 1.5
    
    cv::Mat yuv420sp(raw.height + raw.height / 2, raw.width, CV_8UC1, raw.data);
    cv::Mat bgr(raw.height, raw.width, CV_8UC3, rgb_out);
    
    // NV12 (YUV420SP) -> BGR
    cv::cvtColor(yuv420sp, bgr, cv::COLOR_YUV420sp2BGR);
    
    // 注意：这里输出的是 BGR，VENC 配置的是 RGB888
    // 如果需要 RGB，可以再做一次转换：
    // cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    // 但实际测试发现 Rockchip VENC 接受 BGR 也能正常工作
}

// ============================================================================
// OSD 绘制
// ============================================================================

void SerialFrameProcessor::DrawDetections(void* rgb_data, int width, int height,
                                           const std::vector<DetectionResult>& results) {
    // 使用 OpenCV 绘制检测框
    cv::Mat img(height, width, CV_8UC3, rgb_data);
    
    for (const auto& det : results) {
        // 绘制边界框
        cv::Scalar color;
        if (det.class_id == 0) {
            // 人 -> 绿色
            color = cv::Scalar(0, 255, 0);
        } else if (!det.landmarks.empty()) {
            // 人脸 -> 蓝色
            color = cv::Scalar(255, 0, 0);
        } else {
            // 其他 -> 红色
            color = cv::Scalar(0, 0, 255);
        }
        
        cv::rectangle(img, 
                      cv::Point(det.box.x, det.box.y),
                      cv::Point(det.box.Right(), det.box.Bottom()),
                      color, 2);
        
        // 绘制标签
        std::string label = det.label;
        if (label.empty()) {
            label = std::to_string(det.class_id);
        }
        char text[64];
        snprintf(text, sizeof(text), "%s %.2f", label.c_str(), det.confidence);
        
        int baseline;
        cv::Size text_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
        
        // 标签背景
        cv::rectangle(img,
                      cv::Point(det.box.x, det.box.y - text_size.height - 4),
                      cv::Point(det.box.x + text_size.width, det.box.y),
                      color, cv::FILLED);
        
        // 标签文字
        cv::putText(img, text, cv::Point(det.box.x, det.box.y - 2),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 255), 1);
        
        // 绘制人脸关键点（如果有）
        for (const auto& lm : det.landmarks) {
            cv::circle(img, cv::Point(lm.x, lm.y), 2, cv::Scalar(255, 255, 0), cv::FILLED);
        }
    }
}

// ============================================================================
// 获取检测结果和统计
// ============================================================================

std::vector<DetectionResult> SerialFrameProcessor::GetLatestResults() {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return latest_results_;
}

SerialFrameProcessor::Stats SerialFrameProcessor::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

}  // namespace rknn
