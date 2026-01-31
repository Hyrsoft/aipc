/**
 * @file thread_file.cpp
 * @brief 文件保存线程实现
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#define LOG_TAG "thread_file"

#include "thread_file.h"
#include "common/logger.h"

// ============================================================================
// FileThread 实现
// ============================================================================

FileThread::FileThread(const FileThreadConfig& config)
    : config_(config) {
    
    // 创建 MP4 录制器
    mp4_recorder_ = std::make_unique<Mp4Recorder>(config_.mp4Config);
    
    LOG_INFO("FileThread created");
}

FileThread::~FileThread() {
    Stop();
    LOG_INFO("FileThread destroyed");
}

void FileThread::Start() {
    if (running_) {
        LOG_WARN("FileThread already running");
        return;
    }
    
    running_ = true;
    LOG_INFO("FileThread started");
}

void FileThread::Stop() {
    if (!running_) {
        return;
    }
    
    LOG_INFO("Stopping FileThread...");
    
    // 停止录制
    if (mp4_recorder_ && mp4_recorder_->IsRecording()) {
        mp4_recorder_->StopRecording();
    }
    
    running_ = false;
    LOG_INFO("FileThread stopped");
}

// ============================================================================
// 录制控制接口
// ============================================================================

bool FileThread::StartRecording(const std::string& filename) {
    if (!mp4_recorder_) {
        LOG_ERROR("MP4 recorder not initialized");
        return false;
    }
    
    return mp4_recorder_->StartRecording(filename);
}

void FileThread::StopRecording() {
    if (mp4_recorder_) {
        mp4_recorder_->StopRecording();
    }
}

bool FileThread::IsRecording() const {
    return mp4_recorder_ && mp4_recorder_->IsRecording();
}

std::string FileThread::GetCurrentRecordPath() const {
    if (mp4_recorder_) {
        return mp4_recorder_->GetCurrentFilePath();
    }
    return "";
}

Mp4Recorder::Stats FileThread::GetRecordStats() const {
    if (mp4_recorder_) {
        return mp4_recorder_->GetStats();
    }
    return {};
}

// ============================================================================
// 流消费者接口
// ============================================================================

void FileThread::OnEncodedStream(const EncodedStreamPtr& stream) {
    // 如果正在录制，写入帧
    if (mp4_recorder_ && mp4_recorder_->IsRecording()) {
        mp4_recorder_->WriteFrame(stream);
    }
}

void FileThread::StreamConsumer(EncodedStreamPtr stream, void* user_data) {
    FileThread* self = static_cast<FileThread*>(user_data);
    if (self) {
        self->OnEncodedStream(stream);
    }
}

// ============================================================================
// 全局实例管理
// ============================================================================

static std::unique_ptr<FileThread> g_file_thread;

FileThread* GetFileThread() {
    return g_file_thread.get();
}

void CreateFileThread(const FileThreadConfig& config) {
    if (g_file_thread) {
        LOG_WARN("Global FileThread already exists, destroying old one");
        DestroyFileThread();
    }
    
    g_file_thread = std::make_unique<FileThread>(config);
}

void DestroyFileThread() {
    if (g_file_thread) {
        g_file_thread->Stop();
        g_file_thread.reset();
    }
}