/**
 * @file retinaface_strategy.cpp
 * @brief RetinaFace 人脸检测 - 推理 + OSD 绘制（含人脸关键点）
 */

#include "retinaface_strategy.h"
#include "common/logger.h"

#include "visiong/core/ImageBuffer.h"
#include "visiong/npu/NPU.h"

#include <cstdio>

namespace media {

RetinaFaceStrategy::RetinaFaceStrategy(const std::string& model_path,
                                       float conf_thresh, float nms_thresh)
    : model_path_(model_path),
      conf_thresh_(conf_thresh),
      nms_thresh_(nms_thresh) {}

std::unique_ptr<NPU> RetinaFaceStrategy::CreateNPU() {
    auto npu = std::make_unique<NPU>(
        ModelType::RETINAFACE, model_path_, "", conf_thresh_, nms_thresh_);
    if (!npu->is_initialized()) {
        LOG_ERROR("RetinaFace NPU init failed: model={}", model_path_);
        return nullptr;
    }
    return npu;
}

ImageBuffer RetinaFaceStrategy::ProcessFrame(const ImageBuffer& frame, NPU* npu) {
    auto detections = npu->inference(frame);

    ImageBuffer draw_frame = frame.get_bgr_version().copy();
    for (const auto& det : detections) {
        auto [x, y, w, h] = det.box;
        draw_frame.draw_rectangle(x, y, w, h, {0, 255, 255}, 2, false);

        char text[32];
        snprintf(text, sizeof(text), "%.0f%%", det.score * 100.0f);
        draw_frame.draw_string(x, y - 8, text, {0, 255, 255}, 1.0, 2);

        // 绘制 5 个人脸关键点（左眼、右眼、鼻子、左嘴角、右嘴角）
        for (const auto& [lx, ly] : det.landmarks) {
            draw_frame.draw_circle(
                static_cast<int>(lx), static_cast<int>(ly),
                3, {255, 0, 0}, 2, true);
        }
    }
    return draw_frame;
}

} // namespace media
