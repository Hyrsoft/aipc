/**
 * @file yolov5_strategy.cpp
 * @brief YOLOv5 物体检测 - 推理 + OSD 绘制
 */

#include "yolov5_strategy.h"
#include "common/logger.h"

#include "visiong/core/ImageBuffer.h"
#include "visiong/npu/NPU.h"

#include <cstdio>

namespace media {

YoloV5Strategy::YoloV5Strategy(const std::string& model_path, const std::string& label_path,
                               float conf_thresh, float nms_thresh)
    : model_path_(model_path),
      label_path_(label_path),
      conf_thresh_(conf_thresh),
      nms_thresh_(nms_thresh) {}

std::unique_ptr<NPU> YoloV5Strategy::CreateNPU() {
    auto npu = std::make_unique<NPU>(
        ModelType::YOLOV5, model_path_, label_path_, conf_thresh_, nms_thresh_);
    if (!npu->is_initialized()) {
        LOG_ERROR("YOLOv5 NPU init failed: model={}", model_path_);
        return nullptr;
    }
    return npu;
}

ImageBuffer YoloV5Strategy::ProcessFrame(const ImageBuffer& frame, NPU* npu) {
    auto detections = npu->inference(frame);

    ImageBuffer draw_frame = frame.get_bgr_version().copy();
    for (const auto& det : detections) {
        auto [x, y, w, h] = det.box;
        draw_frame.draw_rectangle(x, y, w, h, {0, 255, 0}, 3, false);

        char text[64];
        snprintf(text, sizeof(text), "%s %.1f%%", det.label.c_str(), det.score * 100.0f);
        draw_frame.draw_string(x, y - 8, text, {0, 255, 0}, 1.0, 2);
    }
    return draw_frame;
}

} // namespace media
