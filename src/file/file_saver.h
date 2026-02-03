/**
 * @file file_saver.h
 * @brief 文件保存器 - MP4 录制
 *
 * 使用纯 RAII 设计：
 * - 构造函数完成初始化
 * - 析构函数完成清理
 * - 无需额外的 init/deinit 调用
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#pragma once

#include <string>
#include <atomic>
#include <memory>
#include <mutex>

// RKMPI 头文件
#include "rk_mpi_venc.h"

// 前向声明
using EncodedStreamPtr = std::shared_ptr<VENC_STREAM_S>;

// ============================================================================
// 配置结构体
// ============================================================================

/**
 * @brief MP4 录制配置
 */
struct Mp4RecordConfig {
    std::string outputDir = "/tmp";       ///< 输出目录
    std::string filenamePrefix = "video"; ///< 文件名前缀
    int width = 1920;                     ///< 视频宽度
    int height = 1080;                    ///< 视频高度
    int fps = 30;                         ///< 帧率
    int gopSize = 60;                     ///< GOP 大小
    int codecType = 8;                    ///< 编码类型：8=H.264, 12=H.265
    int maxDurationSec = 0;               ///< 最大录制时长（秒），0表示无限制
    int64_t maxFileSizeBytes = 0;         ///< 最大文件大小（字节），0表示无限制
};

/**
 * @brief 录制状态
 */
enum class RecordState {
    kIdle,       ///< 空闲
    kRecording,  ///< 录制中
    kStopping    ///< 正在停止
};

// ============================================================================
// MP4 录制器类
// ============================================================================

/**
 * @brief MP4 视频录制器
 * 
 * 使用 FFmpeg 将 H.264/H.265 编码流封装为 MP4 文件
 * RAII 设计：构造即可用，析构自动清理
 */
class Mp4Recorder {
public:
    /**
     * @brief 构造函数
     * @param config 录制配置
     */
    explicit Mp4Recorder(const Mp4RecordConfig& config = Mp4RecordConfig{});
    
    ~Mp4Recorder();

    // 禁用拷贝
    Mp4Recorder(const Mp4Recorder&) = delete;
    Mp4Recorder& operator=(const Mp4Recorder&) = delete;

    /**
     * @brief 开始录制
     * @param filename 可选的文件名（不含路径和扩展名），为空则使用时间戳命名
     * @return true 成功，false 失败
     */
    bool StartRecording(const std::string& filename = "");

    /**
     * @brief 停止录制
     */
    void StopRecording();

    /**
     * @brief 写入编码帧（由流消费者回调调用）
     * @param stream 编码流智能指针
     * @return true 成功，false 失败
     */
    bool WriteFrame(const EncodedStreamPtr& stream);

    /**
     * @brief 获取当前录制状态
     */
    RecordState GetState() const { return state_; }

    /**
     * @brief 检查是否正在录制
     */
    bool IsRecording() const { return state_ == RecordState::kRecording; }

    /**
     * @brief 获取当前录制文件路径
     */
    std::string GetCurrentFilePath() const { return current_file_path_; }

    /**
     * @brief 录制统计信息
     */
    struct Stats {
        uint64_t frames_written = 0;    ///< 已写入帧数
        uint64_t bytes_written = 0;     ///< 已写入字节数
        double duration_sec = 0.0;      ///< 录制时长（秒）
    };
    Stats GetStats() const { return stats_; }

private:
    bool CreateOutputFile(const std::string& filepath);
    void CloseOutputFile();
    std::string GenerateFilename();
    bool SetExtradataFromStream(const uint8_t* data, size_t size);

    Mp4RecordConfig config_;
    std::atomic<RecordState> state_{RecordState::kIdle};
    std::string current_file_path_;
    Stats stats_;
    uint64_t first_pts_ = 0;
    bool extradata_set_ = false;   // 标记是否已设置 SPS/PPS
    bool header_written_ = false;  // 标记是否已写入 header
    std::mutex mutex_;

    // FFmpeg 上下文（使用 void* 避免头文件依赖）
    void* format_ctx_ = nullptr;
    void* codec_ctx_ = nullptr;
};