/**
 * @file ai_engine.cpp
 * @brief AI 推理引擎实现
 *
 * @author 好软，好温暖
 * @date 2026-02-05
 */

#define LOG_TAG "AIEngine"

#include "ai_engine.h"
#include "yolov5_model.h"
#include "retinaface_model.h"
#include "common/logger.h"

namespace rknn {

// ============================================================================
// 工厂函数实现
// ============================================================================

std::unique_ptr<AIModelBase> create_model(ModelType type) {
    switch (type) {
        case ModelType::kYoloV5:
            return std::make_unique<YoloV5Model>();
        case ModelType::kRetinaFace:
            return std::make_unique<RetinaFaceModel>();
        default:
            LOG_ERROR("Unknown model type: {}", static_cast<int>(type));
            return nullptr;
    }
}

// ============================================================================
// AIEngine 实现
// ============================================================================

AIEngine& AIEngine::Instance() {
    static AIEngine instance;
    return instance;
}

AIEngine::AIEngine() {
    LOG_INFO("AIEngine initialized");
}

AIEngine::~AIEngine() {
    UnloadModel();
    LOG_INFO("AIEngine destroyed");
}

int AIEngine::SwitchModel(ModelType type, const ModelConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    LOG_INFO("Switching model to: {} ({})", ModelTypeToString(type), config.model_path);
    
    // 1. 记录旧模型信息（用于日志）
    ModelType old_type = current_type_.load();
    
    // 2. 销毁旧模型（RAII 自动释放资源）
    if (model_) {
        LOG_INFO("Unloading previous model: {}", ModelTypeToString(old_type));
        model_.reset();
        current_type_.store(ModelType::kNone);
    }
    
    // 3. 如果目标类型为 None，直接返回
    if (type == ModelType::kNone) {
        LOG_INFO("Model unloaded, engine is now idle");
        last_input_width_ = 0;
        last_input_height_ = 0;
        return 0;
    }
    
    // 4. 创建新模型
    model_ = create_model(type);
    if (!model_) {
        LOG_ERROR("Failed to create model: {}", ModelTypeToString(type));
        return -1;
    }
    
    // 5. 初始化新模型
    int ret = model_->Init(config);
    if (ret != 0) {
        LOG_ERROR("Failed to initialize model: ret={}", ret);
        model_.reset();
        return -1;
    }
    
    // 6. 更新状态
    current_type_.store(type);
    
    // 7. 检查是否需要重配置 VPSS
    int new_width, new_height;
    model_->GetInputSize(new_width, new_height);
    
    if (vpss_callback_ && (new_width != last_input_width_ || new_height != last_input_height_)) {
        LOG_INFO("Model input size changed: {}x{} -> {}x{}, reconfiguring VPSS",
                 last_input_width_, last_input_height_, new_width, new_height);
        
        ret = vpss_callback_(new_width, new_height);
        if (ret != 0) {
            LOG_WARN("VPSS reconfigure callback failed: ret={}", ret);
            // 不因 VPSS 重配置失败而卸载模型
        }
    }
    
    last_input_width_ = new_width;
    last_input_height_ = new_height;
    
    LOG_INFO("Model switched successfully: {} (input: {}x{})",
             ModelTypeToString(type), new_width, new_height);
    
    return 0;
}

void AIEngine::UnloadModel() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (model_) {
        LOG_INFO("Unloading model: {}", ModelTypeToString(current_type_.load()));
        model_.reset();
        current_type_.store(ModelType::kNone);
        last_input_width_ = 0;
        last_input_height_ = 0;
    }
}

bool AIEngine::HasModel() const {
    return current_type_.load() != ModelType::kNone;
}

ModelType AIEngine::GetCurrentModelType() const {
    return current_type_.load();
}

ModelInfo AIEngine::GetModelInfo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (model_) {
        return model_->GetModelInfo();
    }
    
    return ModelInfo{};  // 返回默认值
}

int AIEngine::ProcessFrame(const void* data, int width, int height, DetectionResultList& results) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    results.Clear();
    
    if (!model_) {
        LOG_DEBUG("No model loaded, returning empty results");
        return 0;
    }
    
    // 1. 设置输入
    int ret = model_->SetInput(data, width, height);
    if (ret != 0) {
        LOG_ERROR("SetInput failed: ret={}", ret);
        return -1;
    }
    
    // 2. 执行推理
    ret = model_->Run();
    if (ret != 0) {
        LOG_ERROR("Run failed: ret={}", ret);
        return -1;
    }
    
    // 3. 获取结果
    ret = model_->GetResults(results);
    if (ret != 0) {
        LOG_ERROR("GetResults failed: ret={}", ret);
        return -1;
    }
    
    LOG_DEBUG("ProcessFrame completed: {} detections", results.Count());
    return 0;
}

int AIEngine::GetInputMemory(void** addr, int* size) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!model_) {
        LOG_ERROR("No model loaded");
        return -1;
    }
    
    void* virt_addr = model_->GetInputVirtAddr();
    int mem_size = model_->GetInputMemSize();
    
    if (!virt_addr || mem_size <= 0) {
        LOG_ERROR("Input memory not available");
        return -1;
    }
    
    if (addr) *addr = virt_addr;
    if (size) *size = mem_size;
    
    return 0;
}

int AIEngine::RunInference(DetectionResultList& results) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    results.Clear();
    
    if (!model_) {
        LOG_ERROR("No model loaded");
        return -1;
    }
    
    // 直接执行推理（假设数据已写入）
    int ret = model_->Run();
    if (ret != 0) {
        LOG_ERROR("Run failed: ret={}", ret);
        return -1;
    }
    
    ret = model_->GetResults(results);
    if (ret != 0) {
        LOG_ERROR("GetResults failed: ret={}", ret);
        return -1;
    }
    
    LOG_DEBUG("RunInference completed: {} detections", results.Count());
    return 0;
}

void AIEngine::SetVpssReconfigureCallback(VpssReconfigureCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    vpss_callback_ = std::move(callback);
    LOG_DEBUG("VPSS reconfigure callback set");
}

void AIEngine::SetModelDir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    model_dir_ = dir;
    LOG_INFO("Model directory set: {}", model_dir_);
}

const std::string& AIEngine::GetModelDir() const {
    return model_dir_;
}

int AIEngine::SwitchModel(ModelType type) {
    // 如果是卸载模型，直接调用完整版本
    if (type == ModelType::kNone) {
        return SwitchModel(type, ModelConfig{});
    }
    
    // 构建模型配置
    ModelConfig config;
    
    switch (type) {
        case ModelType::kYoloV5:
            config.model_path = model_dir_ + "/yolov5.rknn";
            config.labels_path = model_dir_ + "/coco_80_labels_list.txt";
            config.nms_threshold = 0.45f;
            config.conf_threshold = 0.25f;
            break;
            
        case ModelType::kRetinaFace:
            config.model_path = model_dir_ + "/retinaface.rknn";
            config.nms_threshold = 0.4f;
            config.conf_threshold = 0.5f;
            break;
            
        default:
            LOG_ERROR("Unsupported model type for auto-config: {}", static_cast<int>(type));
            return -1;
    }
    
    LOG_INFO("Auto-configured model: type={}, path={}", 
             ModelTypeToString(type), config.model_path);
    
    return SwitchModel(type, config);
}

bool AIEngine::GetRequiredInputSize(int& width, int& height) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!model_) {
        width = 0;
        height = 0;
        return false;
    }
    
    model_->GetInputSize(width, height);
    return true;
}

std::string AIEngine::FormatResultLog(const DetectionResult& result, size_t index,
                                       float letterbox_scale,
                                       int letterbox_pad_x,
                                       int letterbox_pad_y) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!model_) {
        return "";
    }
    
    return model_->FormatResultLog(result, index, letterbox_scale, letterbox_pad_x, letterbox_pad_y);
}

}  // namespace rknn
