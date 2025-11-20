#pragma once
#include "IAIEngine.hpp"
#include <memory>

struct RetinaFaceContext;

class RetinaFaceEngine : public IAIEngine {
public:
    RetinaFaceEngine();
    ~RetinaFaceEngine();

    int Init(const std::string& model_path) override;
    int Inference(const cv::Mat& img, std::vector<ObjectDet>& results) override;
    std::string GetName() const override { return "RetinaFace"; }

private:
    std::unique_ptr<RetinaFaceContext> ctx_;
};
