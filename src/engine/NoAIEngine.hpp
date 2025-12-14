#pragma once
#include "IAIEngine.hpp"

namespace aipc::engine {

    class NoAIEngine : public IAIEngine {
    public:
        int Init(const std::string &model_path) override { return 0; }

        int Inference(const cv::Mat &img, std::vector<ObjectDet> &results) override { return 0; }

        std::string GetName() const override { return "NoAIEngine"; }
    };

} // namespace aipc::engine
