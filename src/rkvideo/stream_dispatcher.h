/**
 * @file stream_dispatcher.h
 * @brief 视频流分发器 - 管理 VENC 输出到多个消费者的分发
 *
 * 实现 VENC 编码流到 RTSP、WebRTC、文件录制的分发
 * 
 * 设计要点：
 * - 拷贝模式：获取帧后立即拷贝给每个消费者，然后立即释放 VENC buffer
 * - 完全解耦：每个消费者有独立的数据拷贝，互不影响
 * - 减轻压力：快速释放 VENC buffer，避免 Buffer Pool 耗尽
 *
 * @author 好软，好温暖
 * @date 2026-02-01
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
 * @brief 流消费者回调类型（使用拷贝后的帧数据）
 */
using StreamConsumer = std::function<void(EncodedFramePtr)>;

/**
 * @brief 编码帧队列类型
 */
using EncodedFrameQueue = MediaQueue<EncodedFramePtr>;

/**
 * @brief 视频流分发器
 * 
 * 从 VENC 获取编码流，拷贝后分发给多个消费者（RTSP、WebRTC、文件录制）
 * 每个消费者拥有独立的数据拷贝，互不影响
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
    void RegisterConsumer(const std::string& name, StreamConsumer consumer, size_t queue_size = 5) {
        ConsumerInfo info;
        info.name = name;
        info.callback = consumer;
        info.queue = std::make_unique<EncodedFrameQueue>(queue_size);
        consumers_.push_back(std::move(info));
        LOG_INFO("Registered stream consumer: {} (queue_size={})", name, queue_size);
    }
    
    /**
     * @brief 启动分发器
     * 
     * 启动后会创建：
     * 1. 多个分发线程：为每个消费者分发数据
     * 2. 一个获取线程：从 VENC 获取编码流、拷贝并释放
     */
    void Start() {
        if (running_) return;
        running_ = true;
        
        // 先启动消费者分发线程（确保它们准备好接收数据）
        for (auto& c : consumers_) {
            c.thread = std::thread(&StreamDispatcher::DispatchLoop, this, &c);
            LOG_DEBUG("Started dispatch thread for consumer: {}", c.name);
        }
        
        // 短暂延迟，确保消费者线程就绪
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        // 最后启动获取线程
        fetch_thread_ = std::thread(&StreamDispatcher::FetchLoop, this);
        
        LOG_INFO("StreamDispatcher started with {} consumers (copy mode)", consumers_.size());
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
        std::unique_ptr<EncodedFrameQueue> queue;
        std::thread thread;
    };
    
    /**
     * @brief 获取循环 - 从 VENC 获取编码流、拷贝分发后立即释放
     * 
     * 拷贝模式流程：
     * 1. GetStream 从 VENC 获取编码帧（阻塞等待）
     * 2. 将数据拷贝到 EncodedFrame 结构
     * 3. ReleaseStream 立即释放 VENC buffer
     * 4. 将拷贝后的帧分发给所有消费者
     */
    void FetchLoop() {
        LOG_INFO("Fetch loop started for VENC channel {} (copy mode)", venc_chn_id_);
        
        uint64_t frameCount = 0;
        uint64_t consecutiveErrors = 0;
        
        // 分配 VENC 结构体（与示例代码保持一致，只分配一次）
        VENC_STREAM_S stream;
        stream.pstPack = static_cast<VENC_PACK_S*>(malloc(sizeof(VENC_PACK_S)));
        if (!stream.pstPack) {
            LOG_ERROR("Failed to allocate VENC_PACK_S");
            return;
        }
        
        while (running_) {
            // 使用阻塞模式获取编码流（与示例代码一致）
            // 但为了能响应 stop 信号，使用 1000ms 超时
            RK_S32 ret = RK_MPI_VENC_GetStream(venc_chn_id_, &stream, 1000);
            
            if (ret != RK_SUCCESS) {
                consecutiveErrors++;
                // 连续错误时打印详细信息
                if (consecutiveErrors == 1 || (consecutiveErrors > 3 && consecutiveErrors % 10 == 0)) {
                    // 0x0e = RK_ERR_BUF_EMPTY (缓冲区空，编码器没有产生数据)
                    const char* errMsg = (ret & 0xFF) == 0x0e ? "BUF_EMPTY" : "UNKNOWN";
                    LOG_WARN("VENC GetStream error: ret=0x{:x} ({}), consecutive={}", 
                             ret, errMsg, consecutiveErrors);
                }
                continue;
            }
            
            // 获取数据虚拟地址
            void* data = RK_MPI_MB_Handle2VirAddr(stream.pstPack->pMbBlk);
            if (!data || stream.pstPack->u32Len == 0) {
                RK_MPI_VENC_ReleaseStream(venc_chn_id_, &stream);
                consecutiveErrors++;
                continue;
            }
            
            frameCount++;
            consecutiveErrors = 0;
            
            // 判断是否为关键帧
            bool isKeyFrame = (stream.pstPack->DataType.enH264EType == H264E_NALU_ISLICE ||
                               stream.pstPack->DataType.enH264EType == H264E_NALU_IDRSLICE ||
                               stream.pstPack->DataType.enH265EType == H265E_NALU_ISLICE ||
                               stream.pstPack->DataType.enH265EType == H265E_NALU_IDRSLICE);
            
            // 打印帧信息
            if (frameCount <= 5 || frameCount % 30 == 0) {
                LOG_DEBUG("Got frame #{}, size={} bytes, keyframe={}", 
                         frameCount, stream.pstPack->u32Len, isKeyFrame);
            }
            
            // 为每个消费者创建独立的数据拷贝
            for (auto& c : consumers_) {
                // 创建帧拷贝（每个消费者独立拥有）
                auto frame = std::make_shared<EncodedFrame>(
                    static_cast<const uint8_t*>(data),
                    stream.pstPack->u32Len,
                    stream.pstPack->u64PTS,
                    stream.u32Seq,
                    isKeyFrame
                );
                
                // 推送到消费者队列
                c.queue->push(frame);
            }
            
            // 立即释放 VENC buffer（所有拷贝完成后）
            RK_S32 releaseRet = RK_MPI_VENC_ReleaseStream(venc_chn_id_, &stream);
            if (releaseRet != RK_SUCCESS) {
                LOG_ERROR("RK_MPI_VENC_ReleaseStream failed: ret=0x{:x}", releaseRet);
            }
        }
        
        // 释放分配的内存
        free(stream.pstPack);
        
        LOG_INFO("Fetch loop exited, total frames: {}", frameCount);
    }
    
    /**
     * @brief 分发循环 - 从队列取出数据并调用消费者回调
     */
    void DispatchLoop(ConsumerInfo* consumer) {
        LOG_DEBUG("Dispatch loop started for consumer: {}", consumer->name);
        
        uint64_t processedCount = 0;
        while (running_) {
            EncodedFramePtr frame;
            if (consumer->queue->pop(frame, 1000)) {  // 1秒超时
                if (frame && consumer->callback) {
                    processedCount++;
                    if (processedCount <= 5 || processedCount % 30 == 0) {
                        LOG_DEBUG("Consumer {} processing frame #{}, size={}", 
                                 consumer->name, processedCount, frame->data.size());
                    }
                    consumer->callback(frame);
                }
            }
        }
        
        LOG_DEBUG("Dispatch loop exited for consumer: {}, processed {} frames", 
                 consumer->name, processedCount);
    }
    
    RK_S32 venc_chn_id_;
    std::atomic<bool> running_;
    std::thread fetch_thread_;
    std::vector<ConsumerInfo> consumers_;
};
