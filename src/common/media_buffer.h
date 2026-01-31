/**
 * @file media_buffer.h
 * @brief 媒体资源管理 - 基于 RAII 的 RKMPI 资源封装
 *
 * 提供对 RKMPI 原生结构体的智能指针封装，实现：
 * - 自动资源释放（利用 shared_ptr 的自定义删除器）
 * - 零拷贝共享（多模块共享同一内存块）
 * - 引用计数管理（最后一个使用者释放时自动调用 MPI Release）
 *
 * 设计原则：
 * 不与 SDK 的内存管理机制对抗，而是在其之上建立符合业务逻辑的生命周期管理
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#pragma once

#include <memory>
#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <cstdio>

// RKMPI 头文件
#include "rk_mpi_sys.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_mb.h"

// ============================================================================
// 类型别名定义
// ============================================================================

/**
 * @brief 原始视频帧智能指针 (VI/VPSS 输出)
 * 
 * 用于 AI 推理路径，从 VI 通道获取的 NV12 原始帧
 * 释放时自动调用 RK_MPI_VI_ReleaseChnFrame
 */
using VideoFramePtr = std::shared_ptr<VIDEO_FRAME_INFO_S>;

/**
 * @brief 编码后的视频流智能指针 (VENC 输出)
 * 
 * 用于推流路径，H.264/H.265 编码后的数据包
 * 释放时自动调用 RK_MPI_VENC_ReleaseStream
 */
using EncodedStreamPtr = std::shared_ptr<VENC_STREAM_S>;

// ============================================================================
// 工厂函数 - 创建带自动释放功能的智能指针
// ============================================================================

/**
 * @brief 从 VI 通道获取原始帧并包装为智能指针
 * 
 * @param dev_id VI 设备 ID
 * @param chn_id VI 通道 ID
 * @param timeout_ms 超时时间（毫秒），-1 表示阻塞等待
 * @return VideoFramePtr 成功返回帧指针，失败返回 nullptr
 * 
 * @note 返回的智能指针在所有引用释放后会自动调用 RK_MPI_VI_ReleaseChnFrame
 */
inline VideoFramePtr acquire_video_frame(RK_S32 dev_id, RK_S32 chn_id, RK_S32 timeout_ms = -1) {
    auto frame = new VIDEO_FRAME_INFO_S();
    
    RK_S32 ret = RK_MPI_VI_GetChnFrame(dev_id, chn_id, frame, timeout_ms);
    if (ret != RK_SUCCESS) {
        delete frame;
        return nullptr;
    }
    
    // 创建带自定义删除器的 shared_ptr
    // 当引用计数归零时，自动释放 MPI 资源
    return VideoFramePtr(frame, [dev_id, chn_id](VIDEO_FRAME_INFO_S* p) {
        if (p) {
            RK_MPI_VI_ReleaseChnFrame(dev_id, chn_id, p);
            delete p;
        }
    });
}

/**
 * @brief 从 VENC 通道获取编码流并包装为智能指针
 * 
 * @param chn_id VENC 通道 ID
 * @param timeout_ms 超时时间（毫秒），-1 表示阻塞等待
 * @return EncodedStreamPtr 成功返回流指针，失败返回 nullptr
 * 
 * @note 返回的智能指针在所有引用释放后会自动调用 RK_MPI_VENC_ReleaseStream
 * @note 内部会分配 VENC_PACK_S，也会在释放时一并清理
 */
inline EncodedStreamPtr acquire_encoded_stream(RK_S32 chn_id, RK_S32 timeout_ms = -1) {
    auto stream = new VENC_STREAM_S();
    memset(stream, 0, sizeof(VENC_STREAM_S));
    stream->pstPack = new VENC_PACK_S();
    memset(stream->pstPack, 0, sizeof(VENC_PACK_S));
    
    RK_S32 ret = RK_MPI_VENC_GetStream(chn_id, stream, timeout_ms);
    if (ret != RK_SUCCESS) {
        // 调试：打印错误码（仅在非超时时打印）
        if (ret != RK_ERR_VENC_BUF_EMPTY) {
            printf("[media_buffer] VENC GetStream failed: 0x%x\n", ret);
        }
        delete stream->pstPack;
        delete stream;
        return nullptr;
    }
    
    // 创建带自定义删除器的 shared_ptr
    return EncodedStreamPtr(stream, [chn_id](VENC_STREAM_S* p) {
        if (p) {
            RK_MPI_VENC_ReleaseStream(chn_id, p);
            delete p->pstPack;
            delete p;
        }
    });
}

// ============================================================================
// 辅助函数 - 获取内存地址
// ============================================================================

/**
 * @brief 从视频帧获取虚拟地址
 * 
 * @param frame 视频帧智能指针
 * @return void* 虚拟地址，失败返回 nullptr
 */
inline void* get_frame_vir_addr(const VideoFramePtr& frame) {
    if (!frame) return nullptr;
    return RK_MPI_MB_Handle2VirAddr(frame->stVFrame.pMbBlk);
}

/**
 * @brief 从视频帧获取物理地址
 * 
 * @param frame 视频帧智能指针
 * @return RK_U64 物理地址
 */
inline RK_U64 get_frame_phy_addr(const VideoFramePtr& frame) {
    if (!frame) return 0;
    return RK_MPI_MB_Handle2PhysAddr(frame->stVFrame.pMbBlk);
}

/**
 * @brief 从编码流获取数据虚拟地址
 * 
 * @param stream 编码流智能指针
 * @return void* 虚拟地址，失败返回 nullptr
 */
inline void* get_stream_vir_addr(const EncodedStreamPtr& stream) {
    if (!stream || !stream->pstPack) return nullptr;
    return RK_MPI_MB_Handle2VirAddr(stream->pstPack->pMbBlk);
}

/**
 * @brief 获取编码流的数据长度
 * 
 * @param stream 编码流智能指针
 * @return RK_U32 数据长度（字节）
 */
inline RK_U32 get_stream_length(const EncodedStreamPtr& stream) {
    if (!stream || !stream->pstPack) return 0;
    return stream->pstPack->u32Len;
}

/**
 * @brief 获取编码流的时间戳
 * 
 * @param stream 编码流智能指针
 * @return RK_U64 PTS 时间戳
 */
inline RK_U64 get_stream_pts(const EncodedStreamPtr& stream) {
    if (!stream || !stream->pstPack) return 0;
    return stream->pstPack->u64PTS;
}

// ============================================================================
// 线程安全的媒体队列 - 用于模块间数据分发
// ============================================================================

/**
 * @brief 线程安全的媒体缓冲队列
 * 
 * 用于 VENC -> RTSP/WebRTC 的数据分发
 * 支持有界队列，超出容量时自动丢弃最旧的帧（背压处理）
 * 
 * @tparam T 媒体数据类型（VideoFramePtr 或 EncodedStreamPtr）
 */
template <typename T>
class MediaQueue {
public:
    /**
     * @brief 构造函数
     * @param max_size 队列最大容量，0 表示无限制
     */
    explicit MediaQueue(size_t max_size = 3) : max_size_(max_size), stopped_(false) {}
    
    /**
     * @brief 推送数据到队列
     * 
     * 如果队列已满，会丢弃最旧的数据（FIFO 丢帧策略）
     * 
     * @param item 要推送的数据
     * @return true 成功推送
     * @return false 队列已停止
     */
    bool push(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return false;
        
        // 背压处理：队列满时丢弃最旧的帧
        if (max_size_ > 0 && queue_.size() >= max_size_) {
            queue_.pop();  // shared_ptr 自动释放旧帧
        }
        
        queue_.push(std::move(item));
        cv_.notify_one();
        return true;
    }
    
    /**
     * @brief 从队列取出数据（阻塞）
     * 
     * @param item 输出参数，取出的数据
     * @param timeout_ms 超时时间（毫秒），-1 表示无限等待
     * @return true 成功取出
     * @return false 超时或队列已停止
     */
    bool pop(T& item, int timeout_ms = -1) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        auto predicate = [this] { return !queue_.empty() || stopped_; };
        
        if (timeout_ms < 0) {
            cv_.wait(lock, predicate);
        } else {
            if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), predicate)) {
                return false;  // 超时
            }
        }
        
        if (stopped_ && queue_.empty()) return false;
        
        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }
    
    /**
     * @brief 尝试取出数据（非阻塞）
     * 
     * @param item 输出参数，取出的数据
     * @return true 成功取出
     * @return false 队列为空
     */
    bool try_pop(T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        
        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }
    
    /**
     * @brief 停止队列
     * 
     * 停止后，push 返回 false，阻塞的 pop 会被唤醒并返回 false
     */
    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        cv_.notify_all();
    }
    
    /**
     * @brief 清空队列
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<T> empty;
        std::swap(queue_, empty);
    }
    
    /**
     * @brief 获取当前队列大小
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
    
    /**
     * @brief 检查队列是否为空
     */
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    size_t max_size_;
    bool stopped_;
};

// 常用队列类型别名
using VideoFrameQueue = MediaQueue<VideoFramePtr>;
using EncodedStreamQueue = MediaQueue<EncodedStreamPtr>;

// ============================================================================
// 单帧持有者 - 用于 AI 推理的"覆盖式更新"
// ============================================================================

/**
 * @brief 单帧持有者
 * 
 * 用于 AI 推理场景，只保留最新的一帧
 * 当新帧到来时，旧帧的 shared_ptr 被覆盖，自动释放 Buffer 给 ISP
 * 
 * @tparam T 媒体数据类型
 */
template <typename T>
class LatestFrameHolder {
public:
    /**
     * @brief 更新为最新帧
     * 
     * @param frame 新帧
     */
    void update(T frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = std::move(frame);
        has_new_ = true;
        cv_.notify_one();
    }
    
    /**
     * @brief 获取最新帧（阻塞等待新帧）
     * 
     * @param timeout_ms 超时时间（毫秒），-1 表示无限等待
     * @return T 最新帧，超时返回空指针
     */
    T wait(int timeout_ms = -1) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        auto predicate = [this] { return has_new_; };
        
        if (timeout_ms < 0) {
            cv_.wait(lock, predicate);
        } else {
            if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), predicate)) {
                return nullptr;
            }
        }
        
        has_new_ = false;
        return latest_;
    }
    
    /**
     * @brief 尝试获取最新帧（非阻塞）
     * 
     * @return T 最新帧，无新帧返回空指针
     */
    T try_get() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!has_new_) return nullptr;
        has_new_ = false;
        return latest_;
    }
    
    /**
     * @brief 获取当前帧（不管是否为新帧）
     */
    T current() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return latest_;
    }

private:
    T latest_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool has_new_ = false;
};

using LatestVideoFrame = LatestFrameHolder<VideoFramePtr>;
using LatestEncodedStream = LatestFrameHolder<EncodedStreamPtr>;
