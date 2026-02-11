/**
 * @file serial_frame_processor.h
 * @brief 串行模式帧处理器 - 集成 AI 推理和 OSD 叠加
 *
 * 在串行模式（Mode B: Inference AI）下，负责：
 * 1. 将 NV12 帧转换为 RGB
 * 2. 执行 AI 推理（YOLOv5 / RetinaFace）
 * 3. 叠加 OSD 检测框
 * 4. 返回处理后的 RGB 帧给 VENC
 *
 * 这是从 aipc_old 架构中复用的串行处理逻辑。
 *
 * @author 好软，好温暖
 * @date 2026-02-11
 */

#pragma once

#include "rkvideo/pipeline_mode.h"
#include "ai_types.h"

#include <vector>
#include <mutex>
#include <atomic>

namespace rknn {

/**
 * @brief 串行帧处理器配置
 */
struct SerialProcessorConfig {
    bool enable_inference = true;   ///< 是否启用 AI 推理
    bool enable_osd = true;         ///< 是否启用 OSD 叠加
    int skip_frames = 0;            ///< 跳帧推理（0=每帧都推理）
};

/**
 * @class SerialFrameProcessor
 * @brief 串行帧处理器
 *
 * 提供串行模式下的帧处理回调函数。
 */
class SerialFrameProcessor {
public:
    /**
     * @brief 获取单例实例
     */
    static SerialFrameProcessor& Instance();

    // 禁止拷贝
    SerialFrameProcessor(const SerialFrameProcessor&) = delete;
    SerialFrameProcessor& operator=(const SerialFrameProcessor&) = delete;

    /**
     * @brief 初始化处理器
     * 
     * @param config 配置参数
     * @return true 成功
     */
    bool Init(const SerialProcessorConfig& config = SerialProcessorConfig{});

    /**
     * @brief 获取帧处理回调函数
     * 
     * 返回一个可以传递给 PipelineManager::SwitchMode 的回调函数
     */
    rkvideo::FrameProcessCallback GetProcessCallback();

    /**
     * @brief 设置配置
     */
    void SetConfig(const SerialProcessorConfig& config);

    /**
     * @brief 获取最新的检测结果
     */
    std::vector<DetectionResult> GetLatestResults();

    /**
     * @brief 获取统计信息
     */
    struct Stats {
        uint64_t frames_processed = 0;
        uint64_t inferences_done = 0;
        double avg_process_time_ms = 0.0;
    };
    Stats GetStats() const;

private:
    SerialFrameProcessor() = default;

    /**
     * @brief 核心帧处理函数
     */
    bool ProcessFrame(const rkvideo::RawFrame& raw, rkvideo::ProcessedFrame& processed);

    /**
     * @brief NV12 转 RGB (OpenCV)
     */
    void ConvertNV12ToRGB(const rkvideo::RawFrame& raw, void* rgb_out);

    /**
     * @brief 绘制检测结果 OSD
     */
    void DrawDetections(void* rgb_data, int width, int height,
                        const std::vector<DetectionResult>& results);

    SerialProcessorConfig config_;
    bool initialized_ = false;

    // 跳帧计数
    std::atomic<uint64_t> frame_count_{0};
    uint64_t skip_counter_ = 0;

    // 最新检测结果
    std::mutex results_mutex_;
    std::vector<DetectionResult> latest_results_;

    // 统计
    mutable std::mutex stats_mutex_;
    Stats stats_;
    double total_time_ = 0.0;
};

}  // namespace rknn
