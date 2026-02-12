/**
 * @file retinaface_model.h
 * @brief RetinaFace 模型实现 - 基于 RKNN 的人脸检测
 *
 * 封装 RetinaFace 模型的：
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

#include "../rknn/ai_types.h"
#include "rknn/rknn_api.h"
#include <vector>
#include <string>

// 前向声明
struct OSDBox;

namespace rknn {

/**
 * @class RetinaFaceModel
 * @brief RetinaFace 人脸检测模型
 *
 * 支持人脸检测和 5 点关键点定位，使用 RKNN 进行 NPU 加速推理。
 * 采用零拷贝内存模式以提高性能。
 */
class RetinaFaceModel {
public:
    RetinaFaceModel();
    ~RetinaFaceModel();

    // 禁止拷贝
    RetinaFaceModel(const RetinaFaceModel&) = delete;
    RetinaFaceModel& operator=(const RetinaFaceModel&) = delete;

    // ========================================================================
    // 生命周期管理
    // ========================================================================
    
    int Init(const ModelConfig& config);
    void Deinit();
    bool IsInitialized() const;

    // ========================================================================
    // 推理接口
    // ========================================================================

    int SetInput(const void* data, int width, int height, int stride = 0);
    int SetInputDma(int dma_fd, int size, int width, int height);
    int Run();
    int GetResults(DetectionResultList& results);

    // ========================================================================
    // 信息查询
    // ========================================================================

    ModelInfo GetModelInfo() const;
    ModelType GetType() const { return ModelType::kRetinaFace; }
    void GetInputSize(int& width, int& height) const;
    void* GetInputVirtAddr() const;
    int GetInputMemSize() const;
    
    /// 格式化检测结果日志（包含关键点信息）
    std::string FormatResultLog(const DetectionResult& result, size_t index,
                                 float letterbox_scale = 1.0f,
                                 int letterbox_pad_x = 0,
                                 int letterbox_pad_y = 0) const;

    /// 生成 OSD 显示框（人脸统一使用黄色）
    void GenerateOSDBoxes(const DetectionResultList& results,
                           std::vector<OSDBox>& boxes) const;

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
