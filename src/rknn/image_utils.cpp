/**
 * @file image_utils.cpp
 * @brief 图像处理工具实现 - 基于 RGA 硬件加速
 *
 * 使用 Rockchip RGA (Raster Graphic Acceleration) 硬件进行图像处理，
 * 避免 CPU 内存拷贝导致的 DDR 带宽瓶颈。
 *
 * @author 好软，好温暖
 * @date 2026-02-05
 */

#define LOG_TAG "ImageUtils"

#include "image_utils.h"
#include "common/logger.h"

// RGA 硬件加速库
#include "im2d.h"
#include "rga.h"

// OpenCV-mobile (仅用于检测结果绘制)
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <cstring>
#include <algorithm>

namespace rknn {

// ============================================================================
// ImageProcessor 实现
// ============================================================================

ImageProcessor::~ImageProcessor() {
    Deinit();
}

bool ImageProcessor::Init(int model_width, int model_height) {
    if (initialized_) {
        Deinit();
    }

    model_width_ = model_width;
    model_height_ = model_height;

    // 预分配 RGA 中间缓冲区（用于 NV12->RGB 转换后的全尺寸 RGB 图像）
    // 预分配最大可能的源图像尺寸（1080p）
    constexpr int MAX_SRC_WIDTH = 1920;
    constexpr int MAX_SRC_HEIGHT = 1080;
    temp_rgb_buffer_.resize(MAX_SRC_WIDTH * MAX_SRC_HEIGHT * 3);
    
    // 预分配缩放后的临时缓冲区（模型输入尺寸大小）
    temp_scaled_buffer_.resize(model_width * model_height * 3);

    initialized_ = true;
    
    // 打印 RGA 信息
    LOG_INFO("ImageProcessor initialized with RGA: model size {}x{}", model_width, model_height);
    LOG_DEBUG("RGA version: {}", querystring(RGA_VERSION));
    
    return true;
}

void ImageProcessor::Deinit() {
    if (!initialized_) {
        return;
    }

    temp_rgb_buffer_.clear();
    temp_scaled_buffer_.clear();
    model_width_ = 0;
    model_height_ = 0;
    initialized_ = false;

    LOG_INFO("ImageProcessor deinitialized");
}

int ImageProcessor::ConvertNV12ToModelInput(const void* nv12_data, int src_width, int src_height,
                                             void* rgb_output, LetterboxInfo& letterbox_info) {
    if (!initialized_ || !nv12_data || !rgb_output) {
        LOG_ERROR("Invalid parameters or not initialized");
        return -1;
    }

    // ========================================
    // 使用 RGA 硬件加速进行图像处理
    // 注意：RV1106 的 RGA 不支持 improcess 带偏移的目标区域
    // 所以使用三步法：转换 -> 缩放 -> 手动复制到 letterbox
    // ========================================
    
    IM_STATUS status;
    
    // 计算 letterbox 参数
    float scale_x = static_cast<float>(model_width_) / src_width;
    float scale_y = static_cast<float>(model_height_) / src_height;
    float scale = std::min(scale_x, scale_y);

    int scaled_width = static_cast<int>(src_width * scale);
    int scaled_height = static_cast<int>(src_height * scale);
    
    // 确保尺寸是偶数（RGA 要求）
    scaled_width = scaled_width & ~1;
    scaled_height = scaled_height & ~1;

    int pad_left = (model_width_ - scaled_width) / 2;
    int pad_top = (model_height_ - scaled_height) / 2;

    // 填充 letterbox 信息
    letterbox_info.scale = scale;
    letterbox_info.pad_left = pad_left;
    letterbox_info.pad_top = pad_top;
    letterbox_info.src_width = src_width;
    letterbox_info.src_height = src_height;
    letterbox_info.dst_width = model_width_;
    letterbox_info.dst_height = model_height_;

    // ========================================
    // 步骤 1: 先用黑色填充整个输出缓冲区（letterbox 边框）
    // 对于黑色（全零）填充，memset 效率很高
    // ========================================
    std::memset(rgb_output, 0, model_width_ * model_height_ * 3);

    // ========================================
    // 步骤 2: NV12 -> RGB 颜色转换到临时缓冲区
    // ========================================
    
    // 源缓冲区：NV12 格式
    rga_buffer_t src_nv12 = wrapbuffer_virtualaddr(
        const_cast<void*>(nv12_data), src_width, src_height, RK_FORMAT_YCbCr_420_SP);
    
    // 临时缓冲区：全尺寸 RGB
    rga_buffer_t tmp_rgb = wrapbuffer_virtualaddr(
        temp_rgb_buffer_.data(), src_width, src_height, RK_FORMAT_RGB_888);
    
    // 执行颜色转换
    status = imcvtcolor(src_nv12, tmp_rgb, 
                        RK_FORMAT_YCbCr_420_SP, RK_FORMAT_RGB_888,
                        IM_YUV_TO_RGB_BT601_LIMIT);
    if (status != IM_STATUS_SUCCESS) {
        LOG_ERROR("RGA imcvtcolor failed: {}", imStrError(status));
        return -1;
    }

    // ========================================
    // 步骤 3: 使用 imresize 缩放到 scaled_width x scaled_height
    // ========================================
    
    rga_buffer_t scaled_rgb = wrapbuffer_virtualaddr(
        temp_scaled_buffer_.data(), scaled_width, scaled_height, RK_FORMAT_RGB_888);
    
    status = imresize(tmp_rgb, scaled_rgb);
    if (status != IM_STATUS_SUCCESS) {
        LOG_ERROR("RGA imresize failed: {}", imStrError(status));
        return -1;
    }
    
    // ========================================
    // 步骤 4: 手动复制缩放后的图像到 letterbox 中心
    // ========================================
    uint8_t* dst_ptr = static_cast<uint8_t*>(rgb_output);
    const uint8_t* src_ptr = temp_scaled_buffer_.data();
    
    // 每行复制
    for (int y = 0; y < scaled_height; ++y) {
        // 目标位置：(pad_left, pad_top + y)
        uint8_t* dst_row = dst_ptr + ((pad_top + y) * model_width_ + pad_left) * 3;
        const uint8_t* src_row = src_ptr + y * scaled_width * 3;
        std::memcpy(dst_row, src_row, scaled_width * 3);
    }

    return 0;
}

int ImageProcessor::DrawDetections(void* rgb_data, int width, int height,
                                    const DetectionResultList& results,
                                    const LetterboxInfo& letterbox_info) {
    if (!rgb_data || width <= 0 || height <= 0) {
        return -1;
    }

    // 创建 OpenCV Mat（不复制数据）
    cv::Mat frame(height, width, CV_8UC3, rgb_data);

    // 绘制每个检测结果
    for (size_t i = 0; i < results.Count(); ++i) {
        const auto& result = results.results[i];

        // 映射坐标到原始图像空间
        int x1 = static_cast<int>(result.box.x);
        int y1 = static_cast<int>(result.box.y);
        int x2 = static_cast<int>(result.box.x + result.box.width);
        int y2 = static_cast<int>(result.box.y + result.box.height);

        MapCoordinates(x1, y1, letterbox_info);
        MapCoordinates(x2, y2, letterbox_info);

        // 限制坐标范围
        x1 = std::max(0, std::min(x1, width - 1));
        y1 = std::max(0, std::min(y1, height - 1));
        x2 = std::max(0, std::min(x2, width - 1));
        y2 = std::max(0, std::min(y2, height - 1));

        // 绘制矩形框
        cv::Scalar color(0, 255, 0);  // 绿色
        cv::rectangle(frame, cv::Point(x1, y1), cv::Point(x2, y2), color, 2);

        // 绘制标签和置信度
        char label_text[64];
        snprintf(label_text, sizeof(label_text), "%s %.1f%%",
                 result.label.c_str(), result.confidence * 100);

        // 文字位置（框的左上角上方）
        int text_y = std::max(y1 - 8, 15);
        cv::putText(frame, label_text, cv::Point(x1, text_y),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1);

        // 绘制关键点（如果是人脸检测）
        if (result.landmarks.size() > 0) {
            for (const auto& lm : result.landmarks) {
                int lx = static_cast<int>(lm.x);
                int ly = static_cast<int>(lm.y);
                MapCoordinates(lx, ly, letterbox_info);
                cv::circle(frame, cv::Point(lx, ly), 2, cv::Scalar(0, 0, 255), -1);
            }
        }
    }

    return 0;
}

void ImageProcessor::MapCoordinates(int& x, int& y, const LetterboxInfo& info) {
    // 从模型空间映射回原始图像空间
    // 模型坐标 -> 去掉 padding -> 除以 scale -> 原始坐标
    x = static_cast<int>((x - info.pad_left) / info.scale);
    y = static_cast<int>((y - info.pad_top) / info.scale);
}

}  // namespace rknn
