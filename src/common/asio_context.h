/**
 * @file asio_context.h
 * @brief 全局 Asio IO Context 管理
 *
 * 为单核 CPU (RV1106) 优化的事件驱动架构核心组件。
 * 
 * 设计原则：
 * - 单线程事件循环：所有网络 I/O 在同一个 io_context 中处理
 * - 最小化上下文切换：避免多线程带来的调度开销
 * - 异步优先：所有 I/O 操作使用异步回调模式
 *
 * 线程模型：
 * - 主 IO 线程：运行 io_context::run()，处理所有网络事件
 * - Video Fetch 线程：独立线程，通过 asio::post 投递任务到主 IO 线程
 * - File I/O 线程：独立线程，处理磁盘写入（因为文件 I/O 延迟不可控）
 *
 * @author 好软，好温暖
 * @date 2026-02-04
 */

#pragma once

// Standalone Asio (不依赖 Boost)
// 注意：这些宏也在 CMakeLists.txt 中定义了，使用 ifndef 避免重复定义警告
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#ifndef ASIO_NO_DEPRECATED
#define ASIO_NO_DEPRECATED
#endif

#include <asio.hpp>

#include <memory>
#include <thread>
#include <atomic>
#include <functional>

/**
 * @brief 全局 IO Context 单例
 * 
 * 提供统一的事件循环，用于：
 * - HTTP 服务器的异步处理
 * - RTSP/WebRTC 的网络发送
 * - WebSocket 的数据推送
 * - 定时器和信号处理
 */
class IoContext {
public:
    /**
     * @brief 获取全局单例
     */
    static IoContext& Instance() {
        static IoContext instance;
        return instance;
    }

    /**
     * @brief 获取底层 asio::io_context 引用
     */
    asio::io_context& Get() { return io_context_; }

    /**
     * @brief 获取 work guard（防止 io_context 在没有任务时退出）
     */
    auto& GetWorkGuard() { return work_guard_; }

    /**
     * @brief 投递任务到 IO 线程执行
     * 
     * 线程安全，可从任意线程调用
     * 
     * @param handler 要执行的任务
     */
    template<typename Handler>
    void Post(Handler&& handler) {
        asio::post(io_context_, std::forward<Handler>(handler));
    }

    /**
     * @brief 在 IO 线程中延迟执行任务
     * 
     * 如果已经在 IO 线程中，则直接执行；否则投递到 IO 线程
     * 
     * @param handler 要执行的任务
     */
    template<typename Handler>
    void Dispatch(Handler&& handler) {
        asio::dispatch(io_context_, std::forward<Handler>(handler));
    }

    /**
     * @brief 创建定时器
     */
    asio::steady_timer CreateTimer() {
        return asio::steady_timer(io_context_);
    }

    /**
     * @brief 启动 IO 事件循环（在调用线程中运行）
     * 
     * 此函数会阻塞，直到调用 Stop()
     */
    void Run() {
        if (running_.exchange(true)) {
            return;  // 已经在运行
        }
        io_context_.run();
        running_ = false;
    }

    /**
     * @brief 在独立线程中启动 IO 事件循环
     */
    void RunInThread() {
        if (running_.exchange(true)) {
            return;
        }
        io_thread_ = std::thread([this]() {
            io_context_.run();
            running_ = false;
        });
    }

    /**
     * @brief 停止 IO 事件循环
     */
    void Stop() {
        work_guard_.reset();  // 允许 run() 在没有任务时退出
        io_context_.stop();
        
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
    }

    /**
     * @brief 重置 IO Context（用于重新启动）
     */
    void Reset() {
        io_context_.restart();
        work_guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            asio::make_work_guard(io_context_));
    }

    /**
     * @brief 检查是否正在运行
     */
    bool IsRunning() const { return running_.load(); }

private:
    IoContext() 
        : work_guard_(std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            asio::make_work_guard(io_context_))) {}
    
    ~IoContext() {
        Stop();
    }

    // 禁止拷贝
    IoContext(const IoContext&) = delete;
    IoContext& operator=(const IoContext&) = delete;

    asio::io_context io_context_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
    std::thread io_thread_;
    std::atomic<bool> running_{false};
};

/**
 * @brief 获取全局 IO Context 的便捷函数
 */
inline asio::io_context& GetIoContext() {
    return IoContext::Instance().Get();
}

/**
 * @brief 投递任务到 IO 线程的便捷函数
 */
template<typename Handler>
inline void PostToIo(Handler&& handler) {
    IoContext::Instance().Post(std::forward<Handler>(handler));
}
