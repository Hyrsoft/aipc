/**
 * @file thread_file.h
 * @brief 文件保存线程 - 管理视频录制和拍照
 *
 * 独立的线程模块，负责：
 * - MP4 视频录制的启停控制
 * - JPEG 拍照的触发和处理
 * - 预留控制接口供 WebSocket/HTTP API 调用
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <functional>

#include "file/file_saver.h"

// ============================================================================
// 文件保存线程配置
// ============================================================================

struct FileThreadConfig {
    Mp4RecordConfig mp4Config;        ///< MP4 录制配置
    JpegCaptureConfig jpegConfig;     ///< JPEG 拍照配置
};

// ============================================================================
// 文件操作命令（用于线程间通信）
// ============================================================================

enum class FileCommand {
    kNone,
    kStartRecording,
    kStopRecording,
    kTakeSnapshot,
};

// ============================================================================
// 文件保存线程类
// ============================================================================

/**
 * @brief 文件保存线程
 * 
 * 管理 MP4 录制和 JPEG 拍照的后台线程
 * 提供异步命令接口，便于外部控制（如 WebSocket）
 */
class FileThread {
public:
    /**
     * @brief 构造函数
     * @param config 文件线程配置
     */
    explicit FileThread(const FileThreadConfig& config);
    
    ~FileThread();

    // 禁用拷贝
    FileThread(const FileThread&) = delete;
    FileThread& operator=(const FileThread&) = delete;

    /**
     * @brief 启动文件保存线程
     */
    void Start();

    /**
     * @brief 停止文件保存线程
     */
    void Stop();

    /**
     * @brief 检查是否正在运行
     */
    bool IsRunning() const { return running_; }

    // ========================================================================
    // 录制控制接口（可被 WebSocket/HTTP 调用）
    // ========================================================================

    /**
     * @brief 开始录制
     * @param filename 可选的文件名
     * @return true 成功，false 失败
     */
    bool StartRecording(const std::string& filename = "");

    /**
     * @brief 停止录制
     */
    void StopRecording();

    /**
     * @brief 检查是否正在录制
     */
    bool IsRecording() const;

    /**
     * @brief 获取当前录制文件路径
     */
    std::string GetCurrentRecordPath() const;

    /**
     * @brief 获取录制统计信息
     */
    Mp4Recorder::Stats GetRecordStats() const;

    // ========================================================================
    // 拍照控制接口
    // ========================================================================

    /**
     * @brief 触发拍照
     * @param filename 可选的文件名
     * @return true 成功，false 失败
     */
    bool TakeSnapshot(const std::string& filename = "");

    /**
     * @brief 获取最后一张照片的路径
     */
    std::string GetLastPhotoPath() const;

    /**
     * @brief 获取已完成的拍照数量
     */
    uint64_t GetPhotoCount() const;

    // ========================================================================
    // 流消费者接口（供 StreamDispatcher 调用）
    // ========================================================================

    /**
     * @brief 处理编码流帧
     * @param stream 编码流
     */
    void OnEncodedStream(const EncodedStreamPtr& stream);

    /**
     * @brief 获取流消费者回调（用于注册到 StreamDispatcher）
     */
    static void StreamConsumer(EncodedStreamPtr stream, void* user_data);

private:
    void JpegProcessThread();

    FileThreadConfig config_;
    
    std::unique_ptr<Mp4Recorder> mp4_recorder_;
    std::unique_ptr<JpegCapturer> jpeg_capturer_;
    
    std::atomic<bool> running_{false};
    std::thread jpeg_thread_;
};

// ============================================================================
// 全局文件线程实例（可选）
// ============================================================================

/**
 * @brief 获取全局文件线程实例（懒加载）
 * 
 * @note 如果需要多实例，直接创建 FileThread 对象
 */
FileThread* GetFileThread();

/**
 * @brief 创建全局文件线程实例
 * @param config 配置
 */
void CreateFileThread(const FileThreadConfig& config);

/**
 * @brief 销毁全局文件线程实例
 */
void DestroyFileThread();

