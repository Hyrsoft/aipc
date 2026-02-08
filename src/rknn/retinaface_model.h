/**
 * @file retinaface_model.h
 * @brief RetinaFace 模型实现 - 基于 RKNN 的人脸检测
 *
 * 实现 AIModelBase 接口，封装 RetinaFace 模型的：
 * - 模型加载和初始化
 * - NPU 推理执行
 * - 后处理（人脸框解码、关键点、NMS）
 *
 * 参考 Luckfox RKMPI 例程中的 retinaface.cc 实现
 *
 * @author 好软，好温暖
 * @date 2026-02-05
 */

#pragma once

#include "ai_model_base.h"
#include "rknn/rknn_api.h"

namespace rknn {

/**
 * @class RetinaFaceModel
 * @brief RetinaFace 人脸检测模型
 *
 * 支持人脸检测和 5 点关键点定位，使用 RKNN 进行 NPU 加速推理。
 * 采用零拷贝内存模式以提高性能。
 */
class RetinaFaceModel : public AIModelBase {
public:
    RetinaFaceModel();
    ~RetinaFaceModel() override;

    // ========================================================================
    // AIModelBase 接口实现
    // ========================================================================
    
    int Init(const ModelConfig& config) override;
    void Deinit() override;
    bool IsInitialized() const override;

    int SetInput(const void* data, int width, int height, int stride = 0) override;
    int SetInputDma(int dma_fd, int size, int width, int height) override;
    int Run() override;
    int GetResults(DetectionResultList& results) override;

    ModelInfo GetModelInfo() const override;
    ModelType GetType() const override { return ModelType::kRetinaFace; }
    void GetInputSize(int& width, int& height) const override;
    void* GetInputVirtAddr() const override;
    int GetInputMemSize() const override;
    
    /// 格式化检测结果日志（RetinaFace 特化版本，包含关键点信息）
    std::string FormatResultLog(const DetectionResult& result, size_t index,
                                 float letterbox_scale = 1.0f,
                                 int letterbox_pad_x = 0,
                                 int letterbox_pad_y = 0) const override;

private:
    /// 后处理：解码输出并执行 NMS
    int PostProcess(DetectionResultList& results);

private:
    // RKNN 上下文
    rknn_context ctx_ = 0;
    
    // 输入输出张量属性
    rknn_input_output_num io_num_{};
    rknn_tensor_attr* input_attrs_ = nullptr;
    rknn_tensor_attr* output_attrs_ = nullptr;
    
    // 零拷贝内存
    rknn_tensor_mem* input_mem_ = nullptr;
    rknn_tensor_mem* output_mems_[3] = {nullptr, nullptr, nullptr};  // location, scores, landmarks
    
    // 模型信息
    int model_width_ = 0;
    int model_height_ = 0;
    int model_channel_ = 0;
    bool is_quant_ = false;
    
    // 配置参数
    ModelConfig config_;
    
    // 初始化状态
    bool initialized_ = false;
};

}  // namespace rknn
