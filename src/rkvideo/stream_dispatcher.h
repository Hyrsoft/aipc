/**
 * @file stream_dispatcher.h
 * @brief 视频流分发器 - 管理 VENC 输出到多个消费者的分发
 *
 * 实现 VENC 编码流到 RTSP 和 WebRTC 的零拷贝分发
 * 
 * 设计要点：
 * - 零拷贝共享：RTSP 和 WebRTC 拿到的是同一个内存块地址
 * - 解耦释放：任一消费者处理慢不影响其他消费者
 * - 背压处理：队列满时丢弃旧帧，避免 VENC Buffer Pool 耗尽
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#pragma once

#include <thread>
#include <atomic>
#include <vector>
#include <functional>
#include "common/media_buffer.h"
#include "common/logger.h"

#undef LOG_TAG
#define LOG_TAG "dispatcher"

/**
 * @brief 流消费者回调类型
 * 
 * 消费者通过此回调接收编码流
 */
using StreamConsumer = std::function<void(EncodedStreamPtr)>;

/**
 * @brief 视频流分发器
 * 
 * 从 VENC 获取编码流，分发给多个消费者（RTSP、WebRTC 等）
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
     * @param queue_size 消费者队列大小（背压控制）
     */
    void RegisterConsumer(const std::string& name, StreamConsumer consumer, size_t queue_size = 3) {
        consumers_.push_back({name, consumer, std::make_unique<EncodedStreamQueue>(queue_size)});
        LOG_INFO("Registered stream consumer: {}", name);
    }
    
    /**
     * @brief 启动分发器
     * 
     * 启动后会创建：
     * 1. 一个获取线程：从 VENC 获取编码流
     * 2. 多个分发线程：为每个消费者分发数据
     */
    void Start() {
        if (running_) return;
        running_ = true;
        
        // 启动获取线程
        fetch_thread_ = std::thread(&StreamDispatcher::FetchLoop, this);
        
        // 为每个消费者启动分发线程
        for (auto& c : consumers_) {
            c.thread = std::thread(&StreamDispatcher::DispatchLoop, this, &c);
        }
        
        LOG_INFO("StreamDispatcher started with {} consumers", consumers_.size());
    }
    
    /**
     * @brief 停止分发器
     */
    void Stop() {
        if (!running_) return;
        running_ = false;
        
        // 停止所有队列
        for (auto& c : consumers_) {
            c.queue->stop();
        }
        
        // 等待所有线程结束
        if (fetch_thread_.joinable()) {
            fetch_thread_.join();
        }
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
        std::unique_ptr<EncodedStreamQueue> queue;
        std::thread thread;
    };
    
    /**
     * @brief 获取循环 - 从 VENC 获取编码流并分发到各队列
     */
    void FetchLoop() {
        LOG_DEBUG("Fetch loop started for VENC channel {}", venc_chn_id_);
        
        uint64_t frameCount = 0;
        while (running_) {
            // 从 VENC 获取编码流（阻塞）
            auto stream = acquire_encoded_stream(venc_chn_id_, 1000);  // 1秒超时
            
            if (!stream) {
                LOG_WARN("Failed to get stream from VENC channel {}", venc_chn_id_);
                continue;  // 超时或错误，继续尝试
            }
            
            frameCount++;
            if (frameCount % 30 == 1) {  // 每30帧打印一次
                LOG_DEBUG("Got frame #{}, size={} bytes", 
                         frameCount, stream->pstPack ? stream->pstPack->u32Len : 0);
            }
            
            // 分发给所有消费者（零拷贝，只是增加引用计数）
            for (auto& c : consumers_) {
                c.queue->push(stream);  // shared_ptr 拷贝，引用计数 +1
            }
        }
        
        LOG_DEBUG("Fetch loop exited, total frames: {}", frameCount);
    }
    
    /**
     * @brief 分发循环 - 从队列取出数据并调用消费者回调
     */
    void DispatchLoop(ConsumerInfo* consumer) {
        LOG_DEBUG("Dispatch loop started for consumer: {}", consumer->name);
        
        while (running_) {
            EncodedStreamPtr stream;
            if (consumer->queue->pop(stream, 1000)) {  // 1秒超时
                if (stream && consumer->callback) {
                    consumer->callback(stream);
                }
            }
        }
        
        LOG_DEBUG("Dispatch loop exited for consumer: {}", consumer->name);
    }
    
    RK_S32 venc_chn_id_;
    std::atomic<bool> running_;
    std::thread fetch_thread_;
    std::vector<ConsumerInfo> consumers_;
};
