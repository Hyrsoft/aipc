/**
 * @file file_service.h
 * @brief 文件保存服务 - 管理视频录制
 *
 * 独立的服务模块，负责：
 * - MP4 视频录制的启停控制
 * - 预留控制接口供 WebSocket/HTTP API 调用
 *
 * 作为视频编码流的消费者之一，使用 Queued 模式处理，
 * 通过独立线程进行文件写入以避免阻塞主 IO 线程。
 *
 * @author 好软，好温暖
 * @date 2026-02-04
 */

#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <functional>

#include "file/file_saver.h"

// ============================================================================
// 文件保存服务配置
// ============================================================================

struct FileServiceConfig {
    Mp4RecordConfig mp4Config;        ///< MP4 录制配置
};

// ============================================================================
// 文件操作命令（用于线程间通信）
// ============================================================================

enum class FileCommand {
    kNone,
    kStartRecording,
    kStopRecording,
};

// ============================================================================
// 文件保存服务类
// ============================================================================

/**
 * @brief 文件保存服务
 * 
 * 管理 MP4 录制和 JPEG 拍照的后台服务
 * 提供异步命令接口，便于外部控制（如 WebSocket）
 */
class FileService {
public:
    /**
     * @brief 构造函数
     * @param config 文件服务配置
     */
    explicit FileService(const FileServiceConfig& config);
    
    ~FileService();

    // 禁用拷贝
    FileService(const FileService&) = delete;
    FileService& operator=(const FileService&) = delete;

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
    FileServiceConfig config_;
    
    std::unique_ptr<Mp4Recorder> mp4_recorder_;
    
    std::atomic<bool> running_{false};
};

// ============================================================================
// 全局文件服务实例（可选）
// ============================================================================

/**
 * @brief 获取全局文件服务实例（懒加载）
 * 
 * @note 如果需要多实例，直接创建 FileService 对象
 */
FileService* GetFileService();

/**
 * @brief 创建全局文件服务实例
 * @param config 配置
 */
void CreateFileService(const FileServiceConfig& config);

/**
 * @brief 销毁全局文件服务实例
 */
void DestroyFileService();