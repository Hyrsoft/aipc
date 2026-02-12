/**
 * @file ai_types.h
 * @brief AI 模块类型定义 - 统一的检测结果和配置结构
 *
 * 提供解耦具体算法的通用数据结构，支持：
 * - 目标检测结果（YOLOv5、RetinaFace 等）
 * - 人脸关键点（可选）
 * - 模型类型枚举
 *
 * @author 好软，好温暖
 * @date 2026-02-05
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace rknn {

// ============================================================================
// 模型类型枚举
// ============================================================================

/**
 * @enum ModelType
 * @brief 支持的 AI 模型类型
 */
enum class ModelType {
    kNone = 0,        ///< 无模型（未加载）
    kYoloV5,          ///< YOLOv5 目标检测
    kRetinaFace,      ///< RetinaFace 人脸检测
    // 后续可扩展更多模型类型
};

/**
 * @brief 模型类型转字符串
 */
inline const char* ModelTypeToString(ModelType type) {
    switch (type) {
        case ModelType::kNone:       return "none";
        case ModelType::kYoloV5:     return "yolov5";
        case ModelType::kRetinaFace: return "retinaface";
        default:                     return "unknown";
    }
}

/**
 * @brief 字符串转模型类型
 */
inline ModelType StringToModelType(const std::string& name) {
    if (name == "yolov5")      return ModelType::kYoloV5;
    if (name == "retinaface")  return ModelType::kRetinaFace;
    return ModelType::kNone;
}

// ============================================================================
// 检测结果结构体
// ============================================================================

/**
 * @struct BoundingBox
 * @brief 边界框（使用绝对像素坐标）
 */
struct BoundingBox {
    int x;      ///< 左上角 X 坐标
    int y;      ///< 左上角 Y 坐标
    int width;  ///< 宽度
    int height; ///< 高度
    
    /// 计算右边界
    int Right() const { return x + width; }
    
    /// 计算下边界
    int Bottom() const { return y + height; }
    
    /// 计算中心 X
    int CenterX() const { return x + width / 2; }
    
    /// 计算中心 Y
    int CenterY() const { return y + height / 2; }
};

/**
 * @struct Landmark
 * @brief 关键点坐标（用于人脸检测）
 */
struct Landmark {
    int x;
    int y;
};

/**
 * @struct DetectionResult
 * @brief 统一的检测结果结构
 *
 * 解耦具体算法，让 OSD 绘制模块不依赖于 YOLOv5 还是 RetinaFace
 */
struct DetectionResult {
    int class_id;           ///< 类别 ID（YOLO: COCO 类别，RetinaFace: 0=人脸）
    float confidence;       ///< 置信度 [0.0, 1.0]
    BoundingBox box;        ///< 边界框
    std::string label;      ///< 类别名称（如 "person", "car", "face"）
    
    /// 人脸关键点（仅 RetinaFace 使用，5个点：左眼、右眼、鼻子、左嘴角、右嘴角）
    std::vector<Landmark> landmarks;
    
    /// 是否有关键点数据
    bool HasLandmarks() const { return !landmarks.empty(); }
};

/**
 * @struct DetectionResultList
 * @brief 检测结果列表
 */
struct DetectionResultList {
    std::vector<DetectionResult> results;   ///< 检测结果数组
    int frame_id = 0;                       ///< 帧 ID（可选，用于追踪）
    int64_t timestamp_ms = 0;               ///< 时间戳（毫秒）
    
    /// 获取检测数量
    size_t Count() const { return results.size(); }
    
    /// 清空结果
    void Clear() { results.clear(); }
    
    /// 添加结果
    void Add(const DetectionResult& result) { results.push_back(result); }
};

// ============================================================================
// 模型配置结构体
// ============================================================================

/**
 * @struct ModelConfig
 * @brief 模型配置参数
 */
struct ModelConfig {
    std::string model_path;         ///< RKNN 模型文件路径
    std::string labels_path;        ///< 标签文件路径（可选）
    float conf_threshold = 0.25f;   ///< 置信度阈值
    float nms_threshold = 0.45f;    ///< NMS 阈值
    int max_detections = 128;       ///< 最大检测数量
};

/**
 * @struct ModelInfo
 * @brief 模型信息（加载后获取）
 */
struct ModelInfo {
    int input_width = 0;            ///< 模型输入宽度
    int input_height = 0;           ///< 模型输入高度
    int input_channels = 0;         ///< 模型输入通道数
    bool is_quant = false;          ///< 是否为量化模型
    ModelType type = ModelType::kNone; ///< 模型类型
};

// ============================================================================
// 常量定义
// ============================================================================

/// COCO 数据集类别数量
constexpr int kCocoClassNum = 80;

/// 最大检测对象数
constexpr int kMaxDetections = 128;

/// YOLOv5 默认输入尺寸
constexpr int kYolov5DefaultInputSize = 640;

/// RetinaFace 默认输入尺寸
constexpr int kRetinaFaceDefaultInputSize = 640;

/// RetinaFace 人脸关键点数量
constexpr int kRetinaFaceLandmarkNum = 5;

}  // namespace rknn
