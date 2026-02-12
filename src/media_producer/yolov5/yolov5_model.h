/**
 * @file yolov5_model.h
 * @brief YOLOv5 模型实现 - 基于 RKNN 的目标检测
 *
 * 封装 YOLOv5 模型的：
 * - 模型加载和初始化
 * - NPU 推理执行
 * - 后处理（NMS、坐标转换）
 *
 * 参考 Luckfox RKMPI 例程中的 yolov5.cc 实现
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
 * @class YoloV5Model
 * @brief YOLOv5 目标检测模型
 *
 * 支持 COCO 80 类目标检测，使用 RKNN 进行 NPU 加速推理。
 * 采用零拷贝内存模式以提高性能。
 */
class YoloV5Model {
public:
    YoloV5Model();
    ~YoloV5Model();

    // 禁止拷贝
    YoloV5Model(const YoloV5Model&) = delete;
    YoloV5Model& operator=(const YoloV5Model&) = delete;

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
    ModelType GetType() const { return ModelType::kYoloV5; }
    void GetInputSize(int& width, int& height) const;
    void* GetInputVirtAddr() const;
    int GetInputMemSize() const;
    
    /// 格式化检测结果日志
    std::string FormatResultLog(const DetectionResult& result, size_t index,
                                 float letterbox_scale = 1.0f,
                                 int letterbox_pad_x = 0,
                                 int letterbox_pad_y = 0) const;

    /// 生成 OSD 显示框（根据 COCO 类别显示不同颜色）
    void GenerateOSDBoxes(const DetectionResultList& results,
                           std::vector<OSDBox>& boxes) const;

private:
    /// 加载标签文件
    int LoadLabels(const std::string& labels_path);
    
    /// 后处理：解码输出并执行 NMS
    int PostProcess(DetectionResultList& results);
    
    /// 处理单个输出层
    int ProcessOutput(int8_t* output, int* anchor, int grid_h, int grid_w,
                      int stride, std::vector<float>& boxes,
                      std::vector<float>& scores, std::vector<int>& class_ids);

private:
    // RKNN 上下文
    rknn_context ctx_ = 0;
    
    // 输入输出张量属性
    rknn_input_output_num io_num_{};
    rknn_tensor_attr* input_attrs_ = nullptr;
    rknn_tensor_attr* output_attrs_ = nullptr;
    
    // 零拷贝内存
    rknn_tensor_mem* input_mem_ = nullptr;
    rknn_tensor_mem* output_mems_[3] = {nullptr, nullptr, nullptr};
    
    // 模型信息
    int model_width_ = 0;
    int model_height_ = 0;
    int model_channel_ = 0;
    bool is_quant_ = false;
    
    // 配置参数
    ModelConfig config_;
    
    // 标签名称
    std::vector<std::string> labels_;
    
    // 初始化状态
    bool initialized_ = false;
};

}  // namespace rknn
