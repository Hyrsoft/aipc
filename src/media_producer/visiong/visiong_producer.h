/**
 * @file visiong_producer.h
 * @brief VisionG Python 驱动模式生产者
 *
 * 架构说明：
 *   Python 脚本全权负责：
 *     - 摄像头初始化（visiong.Camera）
 *     - 帧循环驱动（while aipc.is_running()）
 *     - 模型加载与推理
 *     - 分辨率配置
 *     - 将处理后的帧通过 aipc.submit_frame(frame) 提交给 C++ 编码
 *
 *   C++ 负责：
 *     - Python 解释器生命周期管理（pybind11 embedded interpreter）
 *     - Python 工程的加载、验证与热更新
 *     - 接收 aipc.submit_frame() 推入的帧，送 VENC 编码
 *     - 流媒体分发（RTSP / WebRTC / WebSocket 等）
 *
 * Python 脚本契约：
 *   init()      可选，C++ 在启动前调用一次（初始化摄像头、加载模型等资源）
 *   run()       必须，C++ 在后台线程中调用；脚本在此驱动帧循环，
 *               直至 aipc.is_running() 返回 False
 *   cleanup()   可选，C++ 在停止后调用一次（释放摄像头、模型等资源）
 *
 * 典型 Python 脚本结构：
 * @code
 *   import visiong
 *   import aipc
 *
 *   _cam = None
 *   _detector = None
 *
 *   def init():
 *       global _cam, _detector
 *       _cam = visiong.Camera(640, 360, format='rgb')
 *       _cam.skip(8)
 *       _detector = visiong.NPU('yolov5', MODEL_PATH, LABEL_PATH)
 *
 *   def run():
 *       while aipc.is_running():
 *           frame = _cam.snapshot()
 *           if not frame.is_valid():
 *               continue
 *           out = frame.to_format('bgr888')
 *           # ... 推理 + 绘制 ...
 *           aipc.submit_frame(out)   # 提交给 C++ VENC 编码
 *
 *   def cleanup():
 *       global _cam, _detector
 *       if _cam:
 *           _cam.release()
 *           _cam = None
 *       _detector = None
 * @endcode
 *
 * aipc 模块说明：
 *   aipc.submit_frame(frame)  将处理后的 ImageBuffer 推入 C++ 编码队列
 *   aipc.is_running()         返回 bool，False 表示应退出帧循环
 *
 * 并发安全：
 *   - impl_->runtime 为 shared_ptr，UpdateCode 重载时通过 stop/load/start
 *     三步原子切换，不在 run() 执行中途替换脚本状态。
 *   - g_submit_frame_cb 由全局 mutex 保护，Init/Deinit 时设置/清空。
 */

#pragma once

#include "../i_media_producer.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class ImageBuffer;

namespace media {

    // 预热内嵌 Python 解释器与 visiong 模块，建议在主线程启动早期调用一次。
    void WarmupVisionGPythonRuntime();

    /**
     * @class VisionGProducer
     * @brief Python 驱动帧循环的 VisionG 生产者
     *
     * 内部流水线：
     *   Python run() 调用 visiong.Camera.snapshot()
     *     --> Python 推理 + 绘制
     *     --> aipc.submit_frame(frame)
     *     --> C++ VencManager::encodeToVideo()
     *     --> SerialStreamDispatcher::DispatchFrame()
     *     --> 各流消费者（RTSP / WebRTC / ...）
     *
     * 热更新流程（UpdateCode）：
     *   1. 向 aipc.is_running() 发送 false，等待 Python run() 返回
     *   2. 调用 PythonRuntime::LoadCode()：旧 cleanup() → exec → 验签名 → 新 init()
     *   3. 重新启动后台线程，调用新 run()
     */
    class VisionGProducer : public IMediaProducer {
    public:
        explicit VisionGProducer(const ProducerConfig &config);

        ~VisionGProducer() override;

        // ========== IMediaProducer 接口实现 ==========

        int Init() override;
        int Deinit() override;
        bool Start() override;
        void Stop() override;

        void RegisterStreamConsumer(const std::string &name, StreamCallback callback,
                                    StreamConsumerType type = StreamConsumerType::AsyncIO, int queue_size = 3) override;

        void ClearStreamConsumers() override;

        bool IsInitialized() const override { return initialized_.load(); }
        bool IsRunning() const override { return running_.load(); }
        const char *GetTypeName() const override { return type_name_.c_str(); }
        const ProducerConfig &GetConfig() const override { return config_; }

        // ========== VisionG 专有接口 ==========

        /**
         * @brief 获取当前已加载的 Python 脚本代码
         */
        std::string GetCurrentCode() const;

        /**
         * @brief 获取最近一次错误信息
         */
        std::string GetLastError() const;

        /**
         * @brief 热更新 Python 脚本
         *
         * 执行流程：
         *   1. 停止当前 run() 线程（向 aipc.is_running() 发送 false）
         *   2. 调用 PythonRuntime::LoadCode()（旧 cleanup → exec → 新 init）
         *   3. 若加载成功且之前在运行，重新启动 run() 线程
         *
         * @param code 新的 Python 脚本代码
         * @return 空字符串表示成功；非空为带分类前缀的错误描述
         *         ([exec error] / [signature error] / [init error])
         */
        std::string UpdateCode(const std::string &code);

    private:
        // 禁止拷贝
        VisionGProducer(const VisionGProducer &) = delete;
        VisionGProducer &operator=(const VisionGProducer &) = delete;

        /**
         * @brief 后台线程入口：调用 Python 脚本的 run() 函数（阻塞直到返回）
         */
        void RunPythonScript();

        /**
         * @brief 将帧送入 VENC 编码并分发给所有消费者
         *
         * 由 aipc.submit_frame() 回调调用。
         */
        void EncodeAndDispatch(const ImageBuffer &frame);

    private:
        ProducerConfig config_;
        std::string type_name_;

        std::atomic<bool> initialized_{false};
        std::atomic<bool> running_{false};

        std::thread script_thread_;

        // 内部实现（PIMPL，隐藏 pybind11 / VencManager 依赖）
        struct Impl;
        std::unique_ptr<Impl> impl_;

        mutable std::mutex state_mutex_;
        std::string current_code_;
        std::string last_error_;

        // 统计
        std::atomic<uint64_t> frame_count_{0};
        std::atomic<uint64_t> encode_count_{0};
    };

} // namespace media
