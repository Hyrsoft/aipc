#include "AIManager.hpp"
#include "YoloV5Engine.hpp"
#include "RetinaFaceEngine.hpp"
#include "NoAIEngine.hpp"
#include <iostream>

void AIManager::SwitchModel(ModelType type, const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_); // 🔒 核心：加锁，防止切换时正在推理导致Crash

    std::cout << "Switching AI Model..." << std::endl;

    // 1. 智能指针自动释放旧模型 (触发 rknn_destroy)
    current_engine_.reset(); 
    
    // 2. 创建新引擎实例
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

    // 3. 初始化模型 (如果需要加载文件)
    if (type != ModelType::NONE) {
        std::string model_path = path;
        // 如果未指定路径，使用默认路径 (硬编码兜底)
        if (model_path.empty()) {
            if (type == ModelType::YOLOV5) model_path = "./model/yolov5.rknn";
            else if (type == ModelType::RETINAFACE) model_path = "./model/retinaface.rknn";
        }

        if (new_engine->Init(model_path) != 0) {
            std::cerr << "[Error] Failed to load model: " << model_path << ". Fallback to NoAI." << std::endl;
            current_engine_ = std::make_unique<NoAIEngine>();
            return;
        }
    }

    // 4. 替换当前引擎
    current_engine_ = std::move(new_engine);
    std::cout << "[Nexus] Model Switched Successfully!" << std::endl;
}
