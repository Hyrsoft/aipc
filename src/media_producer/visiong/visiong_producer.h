/**
 * @file visiong_producer.h
 * @brief VisionG 库驱动的 AI 推理模式生产者
 *
 * 使用 VisionG 库替代手动 MPI/RKNN 管理：
 * - Camera 类替代 ISP/VI 初始化和取帧
 * - NPU 类替代 RKNN 上下文管理和推理
 * - ImageBuffer 替代 OpenCV 进行 OSD 绘制
 * - VENC 编码仍使用现有 MPI 接口（保持 EncodedStreamPtr 兼容性）
 *
 * 数据流：
 *   Camera::snapshot() → ImageBuffer (NV12)
 *     → NPU::inference() → vector<Detection>
 *     → ImageBuffer::draw_rectangle/draw_string (OSD)
 *     → 转 RGB → VENC → H264 编码流
 *
 * @author AI Assistant
 * @date 2026-03-20
 */

#pragma once

#include "../i_media_producer.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 前向声明 VisionG 类型
class Camera;
class NPU;
class ImageBuffer;
enum class ModelType;

namespace media {

/**
 * @class VisionGProducer
 * @brief VisionG 库驱动的 AI 推理模式生产者
 *
 * 统一实现 YOLOv5 和 RetinaFace 模式，通过 ModelType 参数区分。
 */
class VisionGProducer : public IMediaProducer {
public:
    /**
     * @brief 构造函数
     * @param config 配置参数
     * @param model_type VisionG NPU 模型类型 (YOLOV5 / RETINAFACE)
     * @param model_path 模型文件路径
     * @param label_path 标签文件路径（可选）
     */
    VisionGProducer(const ProducerConfig& config,
                    ModelType model_type,
                    const std::string& model_path,
                    const std::string& label_path = "");

    ~VisionGProducer() override;

    // ========== IMediaProducer 接口实现 ==========

    int Init() override;
    int Deinit() override;
    bool Start() override;
    void Stop() override;

    void RegisterStreamConsumer(const std::string& name, StreamCallback callback,
                                StreamConsumerType type = StreamConsumerType::AsyncIO,
                                int queue_size = 3) override;

    void ClearStreamConsumers() override;

    bool IsInitialized() const override { return initialized_.load(); }
    bool IsRunning() const override { return running_.load(); }
    const char* GetTypeName() const override { return type_name_.c_str(); }
    const ProducerConfig& GetConfig() const override { return config_; }

    int SetResolution(Resolution preset) override;
    int SetFrameRate(int fps) override;

private:
    // 禁止拷贝
    VisionGProducer(const VisionGProducer&) = delete;
    VisionGProducer& operator=(const VisionGProducer&) = delete;

    /**
     * @brief 初始化 VENC（用于 H264 编码输出）
     */
    int InitVenc();

    /**
     * @brief 反初始化 VENC
     */
    void DeinitVenc();

    /**
     * @brief 帧处理主循环
     */
    void FrameLoop();

private:
    ProducerConfig config_;
    std::string type_name_;

    // VisionG 组件
    ModelType model_type_;
    std::string model_path_;
    std::string label_path_;
    std::unique_ptr<Camera> camera_;
    std::unique_ptr<NPU> npu_;

    // VENC 状态
    bool venc_enabled_ = false;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};

    std::thread frame_thread_;

    // 内部实现
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // 统计
    std::atomic<uint64_t> frame_count_{0};
    std::atomic<uint64_t> inference_count_{0};
};

} // namespace media
