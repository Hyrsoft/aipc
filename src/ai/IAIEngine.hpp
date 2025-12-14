#pragma once

#include <string>
#include <vector>

#include "Types.h"

namespace aipc::ai {

    class IAIEngine {
    public:
        virtual ~IAIEngine() = default;

        virtual int Init(const std::string &model_path) = 0;
        virtual int Inference(const cv::Mat &img, std::vector<ObjectDet> &results) = 0;
        virtual std::string GetName() const = 0;
    };

} // namespace aipc::ai
