#pragma once

#include <memory>

#include "ai/IAIEngine.hpp"

namespace aipc::ai {

    struct RetinaFaceContext;

    class RetinaFaceEngine : public IAIEngine {
    public:
        RetinaFaceEngine();
        ~RetinaFaceEngine();

        int init(const std::string &model_path) override;
        int inference(const cv::Mat &img, std::vector<ObjectDet> &results) override;
        std::string get_name() const override { return "RetinaFace"; }

    private:
        std::unique_ptr<RetinaFaceContext> ctx_;
    };

} // namespace aipc::ai
