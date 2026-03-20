/**
 * @file yolo11_strategy.h
 * @brief YOLO11 物体检测模型策略
 */

#pragma once

#include "../i_model_strategy.h"

#include <string>

namespace media {

class Yolo11Strategy : public IModelStrategy {
public:
    Yolo11Strategy(const std::string& model_path, const std::string& label_path,
                   float conf_thresh = 0.25f, float nms_thresh = 0.45f);

    std::unique_ptr<NPU> CreateNPU() override;
    ImageBuffer ProcessFrame(const ImageBuffer& frame, NPU* npu) override;
    const char* GetName() const override { return "YOLO11"; }

private:
    std::string model_path_;
    std::string label_path_;
    float conf_thresh_;
    float nms_thresh_;
};

} // namespace media
