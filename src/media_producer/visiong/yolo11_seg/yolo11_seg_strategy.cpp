/**
 * @file yolo11_seg_strategy.cpp
 * @brief YOLO11 实例分割 - 推理 + OSD 绘制（含轮廓线）
 */

#include "yolo11_seg_strategy.h"
#include "common/logger.h"

#include "visiong/core/ImageBuffer.h"
#include "visiong/npu/NPU.h"

#include <cstdio>

namespace media {

Yolo11SegStrategy::Yolo11SegStrategy(const std::string& model_path, const std::string& label_path,
                                     float conf_thresh, float nms_thresh)
    : model_path_(model_path),
      label_path_(label_path),
      conf_thresh_(conf_thresh),
      nms_thresh_(nms_thresh) {}

std::unique_ptr<NPU> Yolo11SegStrategy::CreateNPU() {
    auto npu = std::make_unique<NPU>(
        ModelType::YOLO11_SEG, model_path_, label_path_, conf_thresh_, nms_thresh_);
    if (!npu->is_initialized()) {
        LOG_ERROR("YOLO11-Seg NPU init failed: model={}", model_path_);
        return nullptr;
    }
    return npu;
}

ImageBuffer Yolo11SegStrategy::ProcessFrame(const ImageBuffer& frame, NPU* npu) {
    auto detections = npu->inference(frame);

    ImageBuffer draw_frame = frame.get_bgr_version().copy();
    for (const auto& det : detections) {
        auto [x, y, w, h] = det.box;
        draw_frame.draw_rectangle(x, y, w, h, {0, 255, 0}, 2, false);

        char text[64];
        snprintf(text, sizeof(text), "%s %.1f%%", det.label.c_str(), det.score * 100.0f);
        draw_frame.draw_string(x, y - 8, text, {0, 255, 0}, 1.0, 2);

        // 绘制分割轮廓
        const auto& pts = det.mask_points;
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            auto [x0, y0] = pts[i];
            auto [x1, y1] = pts[i + 1];
            draw_frame.draw_line(
                static_cast<int>(x0), static_cast<int>(y0),
                static_cast<int>(x1), static_cast<int>(y1),
                {255, 0, 255}, 2);
        }
        if (pts.size() > 2) {
            auto [x0, y0] = pts.back();
            auto [x1, y1] = pts.front();
            draw_frame.draw_line(
                static_cast<int>(x0), static_cast<int>(y0),
                static_cast<int>(x1), static_cast<int>(y1),
                {255, 0, 255}, 2);
        }
    }
    return draw_frame;
}

} // namespace media
