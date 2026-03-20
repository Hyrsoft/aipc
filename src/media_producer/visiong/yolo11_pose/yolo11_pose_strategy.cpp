/**
 * @file yolo11_pose_strategy.cpp
 * @brief YOLO11 人体姿态估计 - 推理 + OSD 绘制（含骨架连线）
 */

#include "yolo11_pose_strategy.h"
#include "common/logger.h"

#include "visiong/core/ImageBuffer.h"
#include "visiong/npu/NPU.h"

#include <cstdio>

namespace media {

// COCO 17 关键点骨架连线定义 (起点, 终点)
static constexpr int kSkeletonPairs[][2] = {
    {0, 1}, {0, 2}, {1, 3}, {2, 4},       // 头部
    {5, 6}, {5, 7}, {7, 9}, {6, 8}, {8, 10}, // 上半身
    {5, 11}, {6, 12}, {11, 12},             // 躯干
    {11, 13}, {13, 15}, {12, 14}, {14, 16}  // 下半身
};

Yolo11PoseStrategy::Yolo11PoseStrategy(const std::string& model_path, const std::string& label_path,
                                       float conf_thresh, float nms_thresh)
    : model_path_(model_path),
      label_path_(label_path),
      conf_thresh_(conf_thresh),
      nms_thresh_(nms_thresh) {}

std::unique_ptr<NPU> Yolo11PoseStrategy::CreateNPU() {
    auto npu = std::make_unique<NPU>(
        ModelType::YOLO11_POSE, model_path_, label_path_, conf_thresh_, nms_thresh_);
    if (!npu->is_initialized()) {
        LOG_ERROR("YOLO11-Pose NPU init failed: model={}", model_path_);
        return nullptr;
    }
    return npu;
}

ImageBuffer Yolo11PoseStrategy::ProcessFrame(const ImageBuffer& frame, NPU* npu) {
    auto detections = npu->inference(frame);

    ImageBuffer draw_frame = frame.get_bgr_version().copy();
    for (const auto& det : detections) {
        auto [x, y, w, h] = det.box;
        draw_frame.draw_rectangle(x, y, w, h, {0, 255, 0}, 2, false);

        char text[64];
        snprintf(text, sizeof(text), "person %.1f%%", det.score * 100.0f);
        draw_frame.draw_string(x, y - 8, text, {0, 255, 0}, 1.0, 2);

        const auto& kps = det.keypoints;

        // 绘制关键点
        for (const auto& [kx, ky, ks] : kps) {
            if (ks > 0.3f) {
                draw_frame.draw_circle(
                    static_cast<int>(kx), static_cast<int>(ky),
                    4, {0, 0, 255}, 2, true);
            }
        }

        // 绘制骨架连线
        for (const auto& [a, b] : kSkeletonPairs) {
            if (static_cast<size_t>(a) < kps.size() && static_cast<size_t>(b) < kps.size()) {
                auto [ax, ay, as] = kps[a];
                auto [bx, by, bs] = kps[b];
                if (as > 0.3f && bs > 0.3f) {
                    draw_frame.draw_line(
                        static_cast<int>(ax), static_cast<int>(ay),
                        static_cast<int>(bx), static_cast<int>(by),
                        {0, 255, 255}, 2);
                }
            }
        }
    }
    return draw_frame;
}

} // namespace media
