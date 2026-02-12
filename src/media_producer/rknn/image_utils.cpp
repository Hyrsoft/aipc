/**
 * @file image_utils.cpp
 * @brief 图像处理工具实现 - 基于 OpenCV-mobile
 *
 * 使用 OpenCV-mobile 进行图像处理：
 * - NV12 -> RGB 颜色空间转换
 * - 等比缩放 + letterbox 填充
 *
 * @author 好软，好温暖
 * @date 2026-02-05
 */

#define LOG_TAG "ImageUtils"

#include "image_utils.h"
#include "common/logger.h"

// OpenCV-mobile
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

    // 预分配临时缓冲区不再需要（OpenCV 会管理内存）
    temp_rgb_buffer_.clear();
    temp_scaled_buffer_.clear();

    initialized_ = true;
    
    LOG_INFO("ImageProcessor initialized with OpenCV-mobile: model size {}x{}", model_width, model_height);
    
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
                                             int src_stride, void* rgb_output, LetterboxInfo& letterbox_info) {
    if (!initialized_ || !nv12_data || !rgb_output) {
        LOG_ERROR("Invalid parameters or not initialized");
        return -1;
    }
    
    // 如果未指定 stride，使用 width
    if (src_stride <= 0) {
        src_stride = src_width;
    }

    // ========================================
    // 使用 OpenCV-mobile 进行图像处理
    // 参照 luckfox_pico_rtsp_yolov5 示例
    // ========================================
    
    // 计算 letterbox 参数
    float scale_x = static_cast<float>(model_width_) / src_width;
    float scale_y = static_cast<float>(model_height_) / src_height;
    float scale = std::min(scale_x, scale_y);

    int scaled_width = static_cast<int>(src_width * scale);
    int scaled_height = static_cast<int>(src_height * scale);

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
    // 步骤 1: 创建 NV12 Mat（考虑 stride）
    // NV12 格式：Y 平面 height 行，UV 平面 height/2 行
    // ========================================
    
    // 如果 stride == width，可以直接用连续内存
    // 否则需要按行拷贝处理（暂时假设 stride == width 或接近）
    cv::Mat yuv420sp;
    if (src_stride == src_width) {
        // stride 等于 width，直接使用
        yuv420sp = cv::Mat(src_height + src_height / 2, src_width, CV_8UC1, 
                           const_cast<void*>(nv12_data));
    } else {
        // stride 不等于 width，需要逐行拷贝 Y 和 UV 平面
        // Y 平面: height 行，每行 width 字节（stride 对齐）
        // UV 平面: height/2 行，每行 width 字节（stride 对齐）
        yuv420sp = cv::Mat(src_height + src_height / 2, src_width, CV_8UC1);
        
        const uint8_t* src_ptr = static_cast<const uint8_t*>(nv12_data);
        uint8_t* dst_ptr = yuv420sp.data;
        
        // 拷贝 Y 平面
        for (int y = 0; y < src_height; ++y) {
            std::memcpy(dst_ptr + y * src_width, src_ptr + y * src_stride, src_width);
        }
        
        // 拷贝 UV 平面（从 stride * height 开始）
        const uint8_t* uv_src = src_ptr + src_stride * src_height;
        uint8_t* uv_dst = dst_ptr + src_width * src_height;
        for (int y = 0; y < src_height / 2; ++y) {
            std::memcpy(uv_dst + y * src_width, uv_src + y * src_stride, src_width);
        }
    }

    // ========================================
    // 步骤 2: NV12 -> BGR -> RGB
    // ========================================
    cv::Mat bgr, rgb;
    cv::cvtColor(yuv420sp, bgr, cv::COLOR_YUV420sp2BGR);
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);

    // ========================================
    // 步骤 3: 缩放到目标尺寸
    // ========================================
    cv::Mat scaled;
    cv::resize(rgb, scaled, cv::Size(scaled_width, scaled_height), 0, 0, cv::INTER_LINEAR);

    // ========================================
    // 步骤 4: 创建黑色 letterbox 并复制缩放后的图像到中心
    // ========================================
    cv::Mat letterbox_img(model_height_, model_width_, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Rect roi(pad_left, pad_top, scaled_width, scaled_height);
    scaled.copyTo(letterbox_img(roi));

    // ========================================
    // 步骤 5: 复制到输出缓冲区
    // ========================================
    std::memcpy(rgb_output, letterbox_img.data, model_width_ * model_height_ * 3);

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
