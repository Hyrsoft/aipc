/**
 * @file file_saver.h
 * @brief 文件保存器 - MP4 录制和 JPEG 拍照的核心类
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
 * @brief JPEG 拍照配置
 */
struct JpegCaptureConfig {
    std::string outputDir = "/tmp";       ///< 输出目录
    std::string filenamePrefix = "photo"; ///< 文件名前缀
    int width = 1920;                     ///< 图片宽度
    int height = 1080;                    ///< 图片高度
    int quality = 70;                     ///< JPEG 质量（1-100）
    int rotation = 0;                     ///< 旋转角度（0, 90, 180, 270）
    int viDevId = 0;                      ///< VI 设备 ID
    int viChnId = 0;                      ///< VI 通道 ID
    int vencChnId = 1;                    ///< JPEG VENC 通道 ID（避免与 H.264 冲突）
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

    Mp4RecordConfig config_;
    std::atomic<RecordState> state_{RecordState::kIdle};
    std::string current_file_path_;
    Stats stats_;
    uint64_t first_pts_ = 0;
    std::mutex mutex_;

    // FFmpeg 上下文（使用 void* 避免头文件依赖）
    void* format_ctx_ = nullptr;
    void* codec_ctx_ = nullptr;
};

// ============================================================================
// JPEG 拍照器类
// ============================================================================

/**
 * @brief JPEG 拍照器
 * 
 * 使用 RKMPI JPEG 编码器实现视频快照
 * RAII 设计：构造时创建 VENC 通道，析构时销毁
 */
class JpegCapturer {
public:
    /**
     * @brief 构造函数 - 创建 JPEG VENC 通道
     * @param config 拍照配置
     */
    explicit JpegCapturer(const JpegCaptureConfig& config = JpegCaptureConfig{});
    
    ~JpegCapturer();

    // 禁用拷贝
    JpegCapturer(const JpegCapturer&) = delete;
    JpegCapturer& operator=(const JpegCapturer&) = delete;

    /**
     * @brief 触发拍照
     * @param filename 可选的文件名（不含路径和扩展名），为空则使用时间戳命名
     * @return true 成功触发，false 失败
     */
    bool TakeSnapshot(const std::string& filename = "");

    /**
     * @brief 检查是否有待处理的拍照请求
     */
    bool IsPending() const { return pending_count_ > 0; }

    /**
     * @brief 获取已完成的拍照数量
     */
    uint64_t GetCompletedCount() const { return completed_count_; }

    /**
     * @brief 获取最后一张照片的路径
     */
    std::string GetLastPhotoPath() const { return last_photo_path_; }

    /**
     * @brief 检查是否初始化成功
     */
    bool IsValid() const { return valid_; }

    /**
     * @brief 处理循环（需要在独立线程中运行）
     * 
     * 从 VI 获取帧，通过 TDE 缩放，发送到 JPEG VENC 编码
     * @param running 运行标志，设为 false 时退出循环
     */
    void ProcessLoop(std::atomic<bool>& running);

private:
    std::string GenerateFilename();
    bool SaveJpegData(const void* data, size_t len, const std::string& filepath);

    JpegCaptureConfig config_;
    
    std::atomic<int> pending_count_{0};
    std::atomic<uint64_t> completed_count_{0};
    std::string last_photo_path_;
    std::string pending_filename_;
    std::mutex mutex_;
    
    bool valid_ = false;
};

