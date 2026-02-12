/**
 * @file pipeline_mode.h
 * @brief 视频管道运行模式定义
 *
 * 定义两种工作模式：
 * - Mode A: Pure IPC (Parallel) - 零拷贝硬件绑定
 * - Mode B: Inference AI (Serial) - 软件控制串行流水线
 *
 * @author 好软，好温暖
 * @date 2026-02-11
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace rkvideo {

// ============================================================================
// 分辨率预设
// ============================================================================

/**
 * @brief 支持的分辨率预设
 */
enum class ResolutionPreset {
    R_1080P,    ///< 1920x1080 @ 30fps (IPC 默认)
    R_480P,     ///< 720x480 @ 30fps (推理模式强制)
};

/**
 * @brief 分辨率配置
 */
struct ResolutionConfig {
    int width = 1920;
    int height = 1080;
    int framerate = 30;
};

/**
 * @brief 获取分辨率预设的配置
 */
inline ResolutionConfig GetResolutionConfig(ResolutionPreset preset) {
    switch (preset) {
        case ResolutionPreset::R_1080P:
            return {1920, 1080, 30};
        case ResolutionPreset::R_480P:
            return {720, 480, 30};
        default:
            return {1920, 1080, 30};
    }
}

/**
 * @brief 分辨率预设转字符串
 */
inline const char* ResolutionPresetToString(ResolutionPreset preset) {
    switch (preset) {
        case ResolutionPreset::R_1080P: return "1080p";
        case ResolutionPreset::R_480P:  return "480p";
        default:                        return "unknown";
    }
}

/**
 * @brief 字符串转分辨率预设
 */
inline ResolutionPreset StringToResolutionPreset(const std::string& str) {
    if (str == "1080p" || str == "1920x1080") return ResolutionPreset::R_1080P;
    if (str == "480p" || str == "720x480")    return ResolutionPreset::R_480P;
    return ResolutionPreset::R_1080P;  // 默认
}

// ============================================================================
// 模式定义
// ============================================================================

/**
 * @brief 管道运行模式
 */
enum class PipelineMode {
    /**
     * @brief 纯 IPC 模式（并行/零拷贝）
     * 
     * 链路：VI -> VPSS -> VENC (硬件绑定)
     * 
     * 特点：
     * - 完全硬件绑定，零拷贝
     * - CPU 几乎不参与数据流
     * - 总线压力最大（VI/VPSS/VENC 并行访问 DDR）
     * - 适合无 AI 推理的纯流媒体场景
     */
    PureIPC,

    /**
     * @brief AI 推理模式（串行）
     * 
     * 链路：VI -> VPSS (绑定)，VPSS --(手动获取)--> NPU --(手动发送)--> VENC
     * 
     * 特点：
     * - VPSS 到 VENC 解绑，由软件控制时序
     * - 帧经过 AI 处理后手动送入 VENC
     * - 可在 VENC 之前叠加 OSD
     * - 总线压力分散（串行执行）
     * - 适合需要 AI 推理的场景
     */
    InferenceAI
};

// ============================================================================
// 模式状态
// ============================================================================

/**
 * @brief 当前管道状态
 */
struct PipelineState {
    PipelineMode mode = PipelineMode::PureIPC;  ///< 当前模式
    bool initialized = false;                    ///< 是否已初始化
    bool streaming = false;                      ///< 是否正在推流
    uint64_t frame_count = 0;                   ///< 已处理帧数
    uint64_t mode_switch_count = 0;             ///< 模式切换次数
};

// ============================================================================
// 串行模式帧处理回调
// ============================================================================

/**
 * @brief 原始帧结构（用于串行模式）
 * 
 * 在串行模式下，每一帧都会通过此结构传递给处理回调
 */
struct RawFrame {
    void* data = nullptr;           ///< 帧数据指针（NV12 格式）
    int width = 0;                  ///< 宽度
    int height = 0;                 ///< 高度
    int stride = 0;                 ///< 行跨度
    uint64_t pts = 0;               ///< 时间戳（微秒）
    void* mpi_handle = nullptr;     ///< MPI 内部句柄（用于释放）
};

/**
 * @brief 处理后的帧结构（用于编码）
 * 
 * AI 推理或 OSD 叠加后的 RGB 帧，准备送入 VENC
 */
struct ProcessedFrame {
    void* data = nullptr;           ///< 帧数据指针（RGB888 格式）
    int width = 0;                  ///< 宽度
    int height = 0;                 ///< 高度
    uint64_t pts = 0;               ///< 时间戳（微秒）
    void* mb_handle = nullptr;      ///< MbPool 句柄
};

/**
 * @brief 串行模式帧处理回调
 * 
 * @param raw_frame 从 VPSS 获取的原始帧
 * @param processed_frame [out] 处理后的帧，将被送入 VENC
 * @return true 帧处理成功，送入 VENC；false 跳过这一帧
 * 
 * 回调函数负责：
 * 1. 将 NV12 转换为 RGB
 * 2. 执行 AI 推理（可选）
 * 3. 叠加 OSD（可选）
 * 4. 填充 processed_frame
 */
using FrameProcessCallback = std::function<bool(const RawFrame& raw_frame, 
                                                  ProcessedFrame& processed_frame)>;

}  // namespace rkvideo
