#pragma once

#include "IAIEngine.hpp"

namespace aipc::ai {

    class NoAIEngine : public IAIEngine {
    public:
        int init(const std::string &model_path) override { return 0; }
        int inference(const cv::Mat &img, std::vector<ObjectDet> &results) override { return 0; }
        std::string get_name() const override { return "NoAIEngine"; }
    };

} // namespace aipc::ai
