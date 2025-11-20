#pragma once
#include "IAIEngine.hpp"
#include <memory>

// Forward declaration to hide implementation details
struct YoloV5Context;

class YoloV5Engine : public IAIEngine {
public:
    YoloV5Engine();
    ~YoloV5Engine();

    int Init(const std::string& model_path) override;
    int Inference(const cv::Mat& img, std::vector<ObjectDet>& results) override;
    std::string GetName() const override { return "YoloV5"; }

private:
    std::unique_ptr<YoloV5Context> ctx_;
};
