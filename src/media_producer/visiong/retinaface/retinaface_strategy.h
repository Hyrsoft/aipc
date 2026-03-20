/**
 * @file retinaface_strategy.h
 * @brief RetinaFace 人脸检测模型策略
 */

#pragma once

#include "../i_model_strategy.h"

#include <string>

namespace media {

class RetinaFaceStrategy : public IModelStrategy {
public:
    RetinaFaceStrategy(const std::string& model_path,
                       float conf_thresh = 0.5f, float nms_thresh = 0.4f);

    std::unique_ptr<NPU> CreateNPU() override;
    ImageBuffer ProcessFrame(const ImageBuffer& frame, NPU* npu) override;
    const char* GetName() const override { return "RetinaFace"; }

private:
    std::string model_path_;
    float conf_thresh_;
    float nms_thresh_;
};

} // namespace media
