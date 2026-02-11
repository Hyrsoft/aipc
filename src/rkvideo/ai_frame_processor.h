/**
 * @file ai_frame_processor.h
 * @brief AI 帧处理器 - 用于串行模式的帧处理回调
 *
 * 封装 NV12→RGB 转换、AI 推理、OSD 叠加的完整流程，
 * 提供符合 FrameProcessCallback 签名的处理函数。
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#pragma once

#include "pipeline_mode.h"
#include "rknn/image_utils.h"
#include "rknn/ai_types.h"

#include <vector>
#include <mutex>
#include <atomic>
#include <functional>

namespace rkvideo {

/**
 * @brief AI 帧处理器配置
 */
struct AIFrameProcessorConfig {
    int model_width = 640;          ///< 模型输入宽度
    int model_height = 640;         ///< 模型输入高度
    int skip_frames = 2;            ///< 跳帧数（0=每帧推理）
    bool draw_boxes = true;         ///< 是否绘制检测框
    bool enable_log = false;        ///< 是否输出推理日志
};

/**
 * @class AIFrameProcessor
 * @brief AI 帧处理器
 *
 * 提供串行模式下的帧处理回调函数，实现：
 * 1. NV12 → RGB 颜色空间转换
 * 2. 缩放/letterbox 到模型输入尺寸
 * 3. AI 推理
 * 4. 检测框绘制（可选）
 */
class AIFrameProcessor {
public:
    AIFrameProcessor();
    ~AIFrameProcessor();

    // 禁止拷贝
    AIFrameProcessor(const AIFrameProcessor&) = delete;
    AIFrameProcessor& operator=(const AIFrameProcessor&) = delete;

    /**
     * @brief 初始化处理器
     * 
     * @param config 配置
     * @return true 成功，false 失败
     */
    bool Init(const AIFrameProcessorConfig& config);

    /**
     * @brief 反初始化
     */
    void Deinit();

    /**
     * @brief 检查是否已初始化
     */
    bool IsInitialized() const { return initialized_.load(); }

    /**
     * @brief 处理单帧
     * 
     * 符合 FrameProcessCallback 签名，可直接用于 SerialPipeline。
     * 
     * @param raw_frame 输入帧（NV12 格式）
     * @param processed_frame 输出帧（RGB888 格式，需预分配）
     * @return true 处理成功，false 处理失败（跳过此帧）
     */
    bool ProcessFrame(const RawFrame& raw_frame, ProcessedFrame& processed_frame);

    /**
     * @brief 获取可用于 SerialPipeline 的回调函数
     * 
     * @return FrameProcessCallback 回调函数
     */
    FrameProcessCallback GetCallback();

    /**
     * @brief 获取统计信息
     */
    struct Stats {
        uint64_t frames_processed = 0;
        uint64_t frames_skipped = 0;
        uint64_t total_detections = 0;
        double avg_inference_ms = 0.0;
    };
    Stats GetStats() const;

private:
    /**
     * @brief NV12 转 RGB 全分辨率图像
     */
    bool ConvertNV12ToRGB(const void* nv12_data, int width, int height, int stride,
                          void* rgb_output);

    /**
     * @brief 准备模型输入（缩放 + letterbox）
     */
    bool PrepareModelInput(const void* rgb_data, int width, int height);

    /**
     * @brief 运行 AI 推理
     */
    bool RunInference(rknn::DetectionResultList& results);

    /**
     * @brief 绘制检测框
     */
    void DrawDetections(void* rgb_data, int width, int height,
                        const rknn::DetectionResultList& results);

private:
    AIFrameProcessorConfig config_;
    std::atomic<bool> initialized_{false};
    
    // 图像处理器
    rknn::ImageProcessor image_processor_;
    
    // 模型输入缓冲区
    std::vector<uint8_t> model_input_buffer_;
    
    // letterbox 信息
    rknn::LetterboxInfo letterbox_info_;
    
    // 全分辨率 RGB 临时缓冲区（用于 NV12 转换）
    std::vector<uint8_t> temp_rgb_buffer_;
    
    // 跳帧计数
    std::atomic<uint64_t> frame_counter_{0};
    uint64_t skip_counter_ = 0;
    
    // 统计信息
    mutable std::mutex stats_mutex_;
    Stats stats_;
    double total_inference_time_ = 0.0;
    uint64_t inference_count_ = 0;
};

/**
 * @brief 创建默认的 AI 帧处理回调
 * 
 * 便捷函数，创建一个 AIFrameProcessor 并返回其回调函数。
 * 内部使用静态实例，适合单一模式切换场景。
 * 
 * @param model_width 模型输入宽度
 * @param model_height 模型输入高度
 * @return FrameProcessCallback 回调函数，失败返回 nullptr
 */
FrameProcessCallback CreateAIFrameProcessCallback(int model_width = 640, int model_height = 640);

}  // namespace rkvideo
