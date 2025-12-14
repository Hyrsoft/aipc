#include "AIManager.hpp"

#include "retinaface/RetinaFaceEngine.hpp"
#include "yolov5/YoloV5Engine.hpp"

#include <spdlog/spdlog.h>

namespace aipc::ai {

    void AIManager::SwitchModel(ModelType type, const std::string &path) {
        std::lock_guard<std::mutex> lock(mutex_);

        SPDLOG_INFO("Switching AI Model...");

        current_engine_.reset();

        std::unique_ptr<IAIEngine> new_engine;
        switch (type) {
            case ModelType::YOLOV5:
                new_engine = std::make_unique<YoloV5Engine>();
                break;
            case ModelType::RETINAFACE:
                new_engine = std::make_unique<RetinaFaceEngine>();
                break;
            case ModelType::NONE:
            default:
                new_engine = std::make_unique<NoAIEngine>();
                break;
        }

        if (type != ModelType::NONE) {
            std::string model_path = path;
            if (model_path.empty()) {
                if (type == ModelType::YOLOV5) {
                    model_path = "./model/yolov5.rknn";
                } else if (type == ModelType::RETINAFACE) {
                    model_path = "./model/retinaface.rknn";
                }
            }

            if (new_engine->Init(model_path) != 0) {
                SPDLOG_ERROR("Failed to load model: {}. Fallback to NoAI.", model_path);
                current_engine_ = std::make_unique<NoAIEngine>();
                return;
            }
        }

        current_engine_ = std::move(new_engine);
        SPDLOG_INFO("Model switched successfully");
    }

} // namespace aipc::ai
