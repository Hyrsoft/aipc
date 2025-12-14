#pragma once
#include <memory>
#include <mutex>
#include "IAIEngine.hpp"
#include "NoAIEngine.hpp"
// Forward declarations or include headers for other engines
// We will include them in the cpp file or here if we use them in SwitchModel
// For simplicity, we'll include them here, but we haven't created them yet.
// So I will comment them out for now and uncomment later, or just create empty files first.

namespace aipc::engine {

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
            if (current_engine_)
                return current_engine_->GetName();
            return "None";
        }

    private:
        AIManager() { current_engine_ = std::make_unique<NoAIEngine>(); }

        std::unique_ptr<IAIEngine> current_engine_;
        std::mutex mutex_;
    };

} // namespace aipc::engine
