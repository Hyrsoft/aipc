#pragma once

#include <memory>

#include "ai/IAIEngine.hpp"

namespace aipc::ai {

    struct YoloV5Context;

    class YoloV5Engine : public IAIEngine {
    public:
        YoloV5Engine();
        ~YoloV5Engine();

        int init(const std::string &model_path) override;
        int inference(const cv::Mat &img, std::vector<ObjectDet> &results) override;
        std::string get_name() const override { return "YoloV5"; }

    private:
        std::unique_ptr<YoloV5Context> ctx_;
    };

} // namespace aipc::ai
