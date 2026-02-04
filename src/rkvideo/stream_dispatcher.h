/**
 * @file stream_dispatcher.h
 * @brief 视频流分发器 - 单核优化版本
 *
 * 针对 RV1106 单核 CPU 优化的视频流分发架构。
 * 
 * 设计原则：
 * - 直接分发：在 Fetch 线程中直接调用消费者回调，消除中间队列和线程
 * - 零拷贝共享：所有消费者共享同一个 VENC buffer（shared_ptr 管理）
 * - 异步投递：网络发送等耗时操作通过 asio::post 投递到 IO 线程
 *
 * 线程模型（优化后）：
 *   [Hardware VENC]
 *         |
 *         v
 *   [Video Fetch Thread] ← 唯一的视频处理线程
 *         |
 *         | (直接调用消费者回调)
 *         v
 *   +-----+-----+-----+-----+
 *   |     |     |     |     |
 *  RTSP  WS   WebRTC  File
 * (post) (post) (post) (队列)
 *         |
 *         v
 *   [Main IO Thread] ← asio 事件循环，处理网络发送
 *
 * 相比之前的优化：
 * - 消除 4 个分发线程（每个消费者一个）
 * - 消除 4 个阻塞队列
 * - 减少上下文切换开销
 * - 降低锁竞争
 *
 * @author 好软，好温暖
 * @date 2026-02-04
 */

#pragma once

#include <thread>
#include <atomic>
#include <vector>
#include <functional>
#include "common/media_buffer.h"
#include "common/asio_context.h"
#include "common/logger.h"

#undef LOG_TAG
#define LOG_TAG "dispatcher"

/**
 * @brief 流消费者回调类型
 * 
 * 消费者通过此回调接收编码流（零拷贝共享）
 * 
 * @note 回调必须是非阻塞的！如果需要耗时操作，请使用 asio::post 投递到 IO 线程
 */
using StreamConsumer = std::function<void(EncodedStreamPtr)>;

/**
 * @brief 消费者类型
 */
enum class ConsumerType {
    Direct,     ///< 直接在 Fetch 线程中执行（仅用于极快的操作）
    AsyncIO,    ///< 通过 asio::post 投递到 IO 线程执行（网络发送）
    Queued      ///< 通过队列投递到独立线程（文件写入等阻塞操作）
};

/**
 * @brief 视频流分发器（单核优化版）
 * 
 * 从 VENC 获取编码流，直接分发给消费者。
 * 
 * 特点：
 * - 单线程获取：只有一个 Fetch 线程从 VENC 获取数据
 * - 直接分发：在 Fetch 线程中直接调用消费者回调
 * - 异步网络：网络相关消费者通过 asio::post 异步处理
 * - 队列隔离：文件写入等阻塞操作使用独立队列
 */
class StreamDispatcher {
public:
    /**
     * @brief 构造函数
     * @param venc_chn_id VENC 通道 ID
     */
    explicit StreamDispatcher(RK_S32 venc_chn_id = 0) 
        : venc_chn_id_(venc_chn_id), running_(false) {}
    
    ~StreamDispatcher() {
        Stop();
    }
    
    /**
     * @brief 注册流消费者
     * 
     * @param name 消费者名称（用于日志）
     * @param consumer 消费者回调函数
     * @param type 消费者类型，决定回调的执行方式
     * @param queue_size 队列大小（仅对 Queued 类型有效）
     */
    void RegisterConsumer(const std::string& name, StreamConsumer consumer, 
                          ConsumerType type = ConsumerType::AsyncIO,
                          size_t queue_size = 3) {
        ConsumerInfo info;
        info.name = name;
        info.callback = consumer;
        info.type = type;
        
        // 只有 Queued 类型需要队列和线程
        if (type == ConsumerType::Queued) {
            info.queue = std::make_unique<EncodedStreamQueue>(queue_size);
        }
        
        consumers_.push_back(std::move(info));
        LOG_INFO("Registered stream consumer: {} (type={})", name, 
                 type == ConsumerType::Direct ? "Direct" :
                 type == ConsumerType::AsyncIO ? "AsyncIO" : "Queued");
    }
    
    /**
     * @brief 兼容旧接口的注册方法
     * 
     * @deprecated 请使用新的 RegisterConsumer 方法
     */
    void RegisterConsumer(const std::string& name, StreamConsumer consumer, size_t queue_size) {
        // 默认使用 AsyncIO 类型（网络发送）
        RegisterConsumer(name, consumer, ConsumerType::AsyncIO, queue_size);
    }
    
    /**
     * @brief 启动分发器
     * 
     * 启动后会创建：
     * 1. 一个 Fetch 线程：从 VENC 获取编码流并直接分发
     * 2. Queued 类型消费者的独立处理线程
     */
    void Start() {
        if (running_) return;
        running_ = true;
        
        // 启动 Queued 类型消费者的处理线程
        for (auto& c : consumers_) {
            if (c.type == ConsumerType::Queued && c.queue) {
                c.thread = std::thread(&StreamDispatcher::QueuedConsumerLoop, this, &c);
                LOG_DEBUG("Started queued consumer thread for: {}", c.name);
            }
        }
        
        // 短暂延迟，确保 Queued 消费者线程就绪
        if (HasQueuedConsumers()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        
        // 启动 Fetch 线程
        fetch_thread_ = std::thread(&StreamDispatcher::FetchLoop, this);
        
        LOG_INFO("StreamDispatcher started with {} consumers (optimized for single-core)", 
                 consumers_.size());
    }
    
    /**
     * @brief 停止分发器
     */
    void Stop() {
        if (!running_) return;
        running_ = false;
        
        // 停止 Queued 消费者的队列
        for (auto& c : consumers_) {
            if (c.queue) {
                c.queue->stop();
            }
        }
        
        // 等待 Fetch 线程结束
        if (fetch_thread_.joinable()) {
            fetch_thread_.join();
        }
        
        // 等待 Queued 消费者线程结束
        for (auto& c : consumers_) {
            if (c.thread.joinable()) {
                c.thread.join();
            }
        }
        
        LOG_INFO("StreamDispatcher stopped");
    }
    
    /**
     * @brief 检查是否正在运行
     */
    bool IsRunning() const { return running_; }

private:
    struct ConsumerInfo {
        std::string name;
        StreamConsumer callback;
        ConsumerType type = ConsumerType::AsyncIO;
        std::unique_ptr<EncodedStreamQueue> queue;  // 仅 Queued 类型使用
        std::thread thread;                          // 仅 Queued 类型使用
    };
    
    /**
     * @brief 检查是否有 Queued 类型的消费者
     */
    bool HasQueuedConsumers() const {
        for (const auto& c : consumers_) {
            if (c.type == ConsumerType::Queued) return true;
        }
        return false;
    }
    
    /**
     * @brief Fetch 循环 - 从 VENC 获取编码流并直接分发
     * 
     * 核心优化：
     * - 直接在 Fetch 线程中调用消费者回调
     * - 网络消费者通过 asio::post 异步执行
     * - 消除中间队列和分发线程
     */
    void FetchLoop() {
        LOG_DEBUG("Fetch loop started for VENC channel {}", venc_chn_id_);
        
        uint64_t frameCount = 0;
        uint64_t consecutiveErrors = 0;
        
        while (running_) {
            // 从 VENC 获取编码流（零拷贝，shared_ptr 管理生命周期）
            RK_S32 lastError = 0;
            auto stream = acquire_encoded_stream(venc_chn_id_, 1000, &lastError);
            
            if (!stream) {
                consecutiveErrors++;
                if (consecutiveErrors > 3 && consecutiveErrors % 10 == 0) {
                    LOG_WARN("VENC channel {} consecutive errors: {}, last error: {:#x}", 
                             venc_chn_id_, consecutiveErrors, lastError);
                }
                if (consecutiveErrors == 30) {
                    LOG_ERROR("VENC channel {} failed 30 times, error code: {:#x}. "
                              "Check if camera/VI/VPSS pipeline is working.", 
                              venc_chn_id_, lastError);
                }
                continue;
            }
            
            frameCount++;
            consecutiveErrors = 0;
            
            // 打印帧信息（调试用）
            if (frameCount <= 5 || frameCount % 300 == 0) {
                LOG_DEBUG("Got frame #{}, size={} bytes, ref_count={}", 
                         frameCount, stream->pstPack ? stream->pstPack->u32Len : 0,
                         stream.use_count());
            }
            
            // 直接分发给所有消费者（核心优化点）
            DispatchToConsumers(stream);
            
            // stream 在这里离开作用域后，引用计数 -1
            // 当所有消费者都处理完毕后，最后一个释放时会调用 ReleaseStream
        }
        
        LOG_DEBUG("Fetch loop exited, total frames: {}", frameCount);
    }
    
    /**
     * @brief 分发帧给所有消费者
     * 
     * 根据消费者类型选择不同的分发策略：
     * - Direct: 直接在当前线程调用
     * - AsyncIO: 通过 asio::post 投递到 IO 线程
     * - Queued: 放入消费者专属队列
     */
    void DispatchToConsumers(EncodedStreamPtr stream) {
        for (auto& c : consumers_) {
            if (!c.callback) continue;
            
            switch (c.type) {
                case ConsumerType::Direct:
                    // 直接在 Fetch 线程中执行（仅用于极快的操作）
                    c.callback(stream);
                    break;
                    
                case ConsumerType::AsyncIO:
                    // 投递到 IO 线程执行（网络发送）
                    // 捕获 stream 的拷贝，增加引用计数
                    aipc::PostToIo([callback = c.callback, stream]() {
                        callback(stream);
                    });
                    break;
                    
                case ConsumerType::Queued:
                    // 放入队列，由专属线程处理（文件写入）
                    if (c.queue) {
                        c.queue->push(stream);
                    }
                    break;
            }
        }
    }
    
    /**
     * @brief Queued 消费者处理循环
     * 
     * 仅用于需要独立线程的消费者（如文件写入）
     */
    void QueuedConsumerLoop(ConsumerInfo* consumer) {
        LOG_DEBUG("Queued consumer loop started for: {}", consumer->name);
        
        uint64_t processedCount = 0;
        while (running_) {
            EncodedStreamPtr stream;
            if (consumer->queue->pop(stream, 1000)) {
                if (stream && consumer->callback) {
                    processedCount++;
                    consumer->callback(stream);
                }
            }
        }
        
        LOG_DEBUG("Queued consumer loop exited for: {}, processed {} frames", 
                 consumer->name, processedCount);
    }
    
    RK_S32 venc_chn_id_;
    std::atomic<bool> running_;
    std::thread fetch_thread_;
    std::vector<ConsumerInfo> consumers_;
};
