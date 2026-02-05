/**
 * @file image_utils.cpp
 * @brief 图像处理工具实现
 *
 * @author 好软，好温暖
 * @date 2026-02-05
 */

#define LOG_TAG "ImageUtils"

#include "image_utils.h"
#include "common/logger.h"

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

    // 预分配临时缓冲区
    temp_rgb_buffer_.resize(model_width * model_height * 3);

    initialized_ = true;
    LOG_INFO("ImageProcessor initialized: model size {}x{}", model_width, model_height);
    return true;
}

void ImageProcessor::Deinit() {
    if (!initialized_) {
        return;
    }

    temp_rgb_buffer_.clear();
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

    // 使用 OpenCV 进行颜色转换和缩放
    // NV12: Y 平面 + UV 交错平面
    int nv12_height = src_height + src_height / 2;
    cv::Mat nv12_mat(nv12_height, src_width, CV_8UC1, const_cast<void*>(nv12_data));
    cv::Mat bgr_mat(src_height, src_width, CV_8UC3);

    // NV12 (YUV420SP) -> BGR
    cv::cvtColor(nv12_mat, bgr_mat, cv::COLOR_YUV420sp2BGR);

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

    // 缩放
    cv::Mat scaled_mat;
    cv::resize(bgr_mat, scaled_mat, cv::Size(scaled_width, scaled_height), 0, 0, cv::INTER_LINEAR);

    // 创建 letterbox 图像（黑色填充）
    cv::Mat letterbox_mat(model_height_, model_width_, CV_8UC3, cv::Scalar(0, 0, 0));

    // 将缩放后的图像复制到中心
    cv::Rect roi(pad_left, pad_top, scaled_width, scaled_height);
    scaled_mat.copyTo(letterbox_mat(roi));

    // 复制到输出缓冲区
    std::memcpy(rgb_output, letterbox_mat.data, model_width_ * model_height_ * 3);

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
