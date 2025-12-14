#pragma once

#include <memory>
#include <mutex>

#include "IAIEngine.hpp"
#include "NoAIEngine.hpp"

namespace aipc::ai {

    enum class ModelType { NONE, YOLOV5, RETINAFACE };

    class AIManager {
    public:
        static AIManager &Instance() {
            static AIManager s;
            return s;
        }

        void SwitchModel(ModelType type, const std::string &path = "");

        void RunInference(const cv::Mat &img, std::vector<ObjectDet> &out) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (current_engine_) {
                current_engine_->Inference(img, out);
            }
        }

        std::string GetCurrentModelName() {
            std::lock_guard<std::mutex> lock(mutex_);
            if (current_engine_) {
                return current_engine_->GetName();
            }
            return "None";
        }

    private:
        AIManager() { current_engine_ = std::make_unique<NoAIEngine>(); }

        std::unique_ptr<IAIEngine> current_engine_;
        std::mutex mutex_;
    };

} // namespace aipc::ai
