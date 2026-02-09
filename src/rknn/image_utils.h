/**
 * @file image_utils.h
 * @brief 图像处理工具 - 颜色转换、缩放和标注绘制
 *
 * 基于 OpenCV-mobile 和 RGA 提供图像处理功能：
 * - NV12 -> RGB 颜色空间转换
 * - 图像缩放和 letterbox 填充
 * - 检测框和文字绘制
 *
 * @author 好软，好温暖
 * @date 2026-02-05
 */

#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include "ai_types.h"

namespace rknn {

/**
 * @struct ImageBuffer
 * @brief 图像缓冲区描述
 */
struct ImageBuffer {
    void* data = nullptr;       ///< 数据指针
    int width = 0;              ///< 宽度
    int height = 0;             ///< 高度
    int stride = 0;             ///< 行步长（字节）
    int channels = 3;           ///< 通道数
    
    /**
     * @brief 计算数据大小
     */
    int Size() const {
        return stride > 0 ? stride * height : width * height * channels;
    }
};

/**
 * @struct LetterboxInfo
 * @brief letterbox 变换信息（用于坐标映射）
 */
struct LetterboxInfo {
    float scale = 1.0f;         ///< 缩放比例
    int pad_left = 0;           ///< 左侧填充
    int pad_top = 0;            ///< 顶部填充
    int src_width = 0;          ///< 原始宽度
    int src_height = 0;         ///< 原始高度
    int dst_width = 0;          ///< 目标宽度
    int dst_height = 0;         ///< 目标高度
};

/**
 * @class ImageProcessor
 * @brief 图像处理器
 *
 * 提供 AI 推理所需的图像预处理和后处理功能
 */
class ImageProcessor {
public:
    ImageProcessor() = default;
    ~ImageProcessor();

    /**
     * @brief 初始化处理器
     * 
     * @param model_width 模型输入宽度
     * @param model_height 模型输入高度
     * @return true 成功，false 失败
     */
    bool Init(int model_width, int model_height);

    /**
     * @brief 反初始化
     */
    void Deinit();

    /**
     * @brief 转换 NV12 到 RGB 并缩放到模型输入尺寸
     * 
     * @param nv12_data NV12 数据（Y + UV 平面）
     * @param src_width 源宽度
     * @param src_height 源高度
     * @param rgb_output RGB 输出缓冲区（需预先分配）
     * @param[out] letterbox_info letterbox 信息（用于坐标映射）
     * @return 0 成功，负值失败
     */
    int ConvertNV12ToModelInput(const void* nv12_data, int src_width, int src_height,
                                 void* rgb_output, LetterboxInfo& letterbox_info);

    /**
     * @brief 在 RGB 图像上绘制检测结果
     * 
     * @param rgb_data RGB 图像数据
     * @param width 图像宽度
     * @param height 图像高度
     * @param results 检测结果列表
     * @param letterbox_info letterbox 信息（用于坐标映射）
     * @return 0 成功，负值失败
     */
    int DrawDetections(void* rgb_data, int width, int height,
                       const DetectionResultList& results,
                       const LetterboxInfo& letterbox_info);

    /**
     * @brief 将检测框坐标从模型空间映射回原始图像空间
     * 
     * @param x 输入/输出 X 坐标
     * @param y 输入/输出 Y 坐标
     * @param info letterbox 信息
     */
    static void MapCoordinates(int& x, int& y, const LetterboxInfo& info);

    /**
     * @brief 获取模型输入尺寸
     */
    int GetModelWidth() const { return model_width_; }
    int GetModelHeight() const { return model_height_; }

private:
    int model_width_ = 0;
    int model_height_ = 0;
    bool initialized_ = false;
    
    // 中间缓冲区
    std::vector<uint8_t> temp_rgb_buffer_;     ///< NV12->RGB 转换后的全尺寸缓冲区
    std::vector<uint8_t> temp_scaled_buffer_;  ///< 缩放后的临时缓冲区
};

}  // namespace rknn
