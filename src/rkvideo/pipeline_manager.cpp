/**
 * @file pipeline_manager.cpp
 * @brief 管道模式管理器实现
 *
 * @author 好软，好温暖
 * @date 2026-02-11
 */

#define LOG_TAG "PipeMgr"

#include "pipeline_manager.h"
#include "rkvideo.h"
#include "common/logger.h"

#include <chrono>

namespace rkvideo {

// ============================================================================
// 单例实现
// ============================================================================

PipelineManager& PipelineManager::Instance() {
    static PipelineManager instance;
    return instance;
}

PipelineManager::~PipelineManager() {
    Deinit();
}

// ============================================================================
// 初始化与反初始化
// ============================================================================

int PipelineManager::Init(const PipelineManagerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (state_.initialized) {
        LOG_WARN("Pipeline manager already initialized");
        return 0;
    }
    
    config_ = config;
    
    // 根据初始模式初始化对应的管道
    int ret = 0;
    if (config_.initial_mode == PipelineMode::PureIPC) {
        ret = InitParallelPipeline();
    } else {
        ret = InitSerialPipeline();
    }
    
    if (ret != 0) {
        LOG_ERROR("Failed to initialize pipeline in {} mode",
                  config_.initial_mode == PipelineMode::PureIPC ? "PureIPC" : "InferenceAI");
        return -1;
    }
    
    state_.mode = config_.initial_mode;
    state_.initialized = true;
    
    LOG_INFO("Pipeline manager initialized in {} mode",
             state_.mode == PipelineMode::PureIPC ? "PureIPC (Parallel)" : "InferenceAI (Serial)");
    return 0;
}

int PipelineManager::Deinit() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!state_.initialized) {
        return 0;
    }
    
    Stop();
    
    // 根据当前模式反初始化
    if (state_.mode == PipelineMode::PureIPC) {
        DeinitParallelPipeline();
    } else {
        DeinitSerialPipeline();
    }
    
    state_.initialized = false;
    LOG_INFO("Pipeline manager deinitialized");
    return 0;
}

// ============================================================================
// 启动与停止
// ============================================================================

bool PipelineManager::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!state_.initialized) {
        LOG_ERROR("Pipeline manager not initialized");
        return false;
    }
    
    if (state_.streaming) {
        LOG_WARN("Pipeline already running");
        return true;
    }
    
    if (state_.mode == PipelineMode::PureIPC) {
        // 并行模式：启动流分发
        rkvideo_start_streaming();
    } else {
        // 串行模式：启动帧处理循环
        if (serial_pipeline_) {
            if (!serial_pipeline_->Start(frame_process_callback_)) {
                LOG_ERROR("Failed to start serial pipeline");
                return false;
            }
        }
    }
    
    state_.streaming = true;
    LOG_INFO("Pipeline started in {} mode",
             state_.mode == PipelineMode::PureIPC ? "PureIPC" : "InferenceAI");
    return true;
}

void PipelineManager::Stop() {
    // 注意：不加锁，因为可能在 SwitchMode 中调用
    
    if (!state_.streaming) {
        return;
    }
    
    if (state_.mode == PipelineMode::PureIPC) {
        rkvideo_stop_streaming();
    } else {
        if (serial_pipeline_) {
            serial_pipeline_->Stop();
        }
    }
    
    state_.streaming = false;
    LOG_INFO("Pipeline stopped");
}

// ============================================================================
// 模式切换
// ============================================================================

int PipelineManager::SwitchMode(PipelineMode mode, FrameProcessCallback process_callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!state_.initialized) {
        LOG_ERROR("Pipeline manager not initialized");
        return -1;
    }
    
    if (mode == state_.mode) {
        LOG_DEBUG("Already in {} mode, skip switch",
                  mode == PipelineMode::PureIPC ? "PureIPC" : "InferenceAI");
        // 更新回调（如果提供了）
        if (process_callback && mode == PipelineMode::InferenceAI) {
            frame_process_callback_ = std::move(process_callback);
        }
        return 0;
    }
    
    PipelineMode old_mode = state_.mode;
    
    LOG_INFO(">>> Mode switch: {} -> {} (cold start)",
             old_mode == PipelineMode::PureIPC ? "PureIPC" : "InferenceAI",
             mode == PipelineMode::PureIPC ? "PureIPC" : "InferenceAI");
    
    auto start_time = std::chrono::steady_clock::now();
    
    // 1. 停止当前管道
    bool was_streaming = state_.streaming;
    if (was_streaming) {
        Stop();
    }
    
    // 2. 反初始化当前 MPI 配置
    if (old_mode == PipelineMode::PureIPC) {
        DeinitParallelPipeline();
    } else {
        DeinitSerialPipeline();
    }
    
    // 3. 按新模式重新初始化 MPI
    int ret = 0;
    if (mode == PipelineMode::PureIPC) {
        ret = InitParallelPipeline();
    } else {
        frame_process_callback_ = std::move(process_callback);
        ret = InitSerialPipeline();
    }
    
    if (ret != 0) {
        LOG_ERROR("Failed to reinitialize pipeline in new mode, reverting...");
        // 尝试恢复到旧模式
        if (old_mode == PipelineMode::PureIPC) {
            InitParallelPipeline();
        } else {
            InitSerialPipeline();
        }
        return -1;
    }
    
    state_.mode = mode;
    state_.mode_switch_count++;
    
    // 4. 重新注册流消费者
    ReregisterConsumers();
    
    // 5. 如果之前在运行，重新启动
    if (was_streaming) {
        state_.streaming = false;  // 重置状态
        if (state_.mode == PipelineMode::PureIPC) {
            rkvideo_start_streaming();
        } else {
            if (serial_pipeline_) {
                serial_pipeline_->Start(frame_process_callback_);
            }
        }
        state_.streaming = true;
    }
    
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    LOG_INFO("<<< Mode switch completed in {}ms (switch count: {})",
             duration.count(), state_.mode_switch_count);
    
    // 6. 通知外部模块
    if (mode_switch_callback_) {
        mode_switch_callback_(old_mode, mode);
    }
    
    return 0;
}

// ============================================================================
// 分辨率与帧率设置
// ============================================================================

int PipelineManager::SetResolution(ResolutionPreset preset) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!state_.initialized) {
        LOG_ERROR("Pipeline manager not initialized");
        return -1;
    }
    
    // 推理模式强制 480p
    if (state_.mode == PipelineMode::InferenceAI && preset != ResolutionPreset::R_480P) {
        LOG_WARN("InferenceAI mode forces 480p, ignoring {} request",
                 ResolutionPresetToString(preset));
        return 0;
    }
    
    if (preset == config_.resolution) {
        LOG_DEBUG("Already at {} resolution, skip switch", ResolutionPresetToString(preset));
        return 0;
    }
    
    ResolutionPreset old_resolution = config_.resolution;
    
    LOG_INFO(">>> Resolution switch: {} -> {} (cold start)",
             ResolutionPresetToString(old_resolution),
             ResolutionPresetToString(preset));
    
    auto start_time = std::chrono::steady_clock::now();
    
    // 1. 停止当前管道
    bool was_streaming = state_.streaming;
    if (was_streaming) {
        Stop();
    }
    
    // 2. 反初始化当前 MPI 配置
    if (state_.mode == PipelineMode::PureIPC) {
        DeinitParallelPipeline();
    } else {
        DeinitSerialPipeline();
    }
    
    // 3. 更新分辨率配置
    config_.ApplyResolution(preset);
    
    // 4. 按当前模式重新初始化 MPI
    int ret = 0;
    if (state_.mode == PipelineMode::PureIPC) {
        ret = InitParallelPipeline();
    } else {
        ret = InitSerialPipeline();
    }
    
    if (ret != 0) {
        LOG_ERROR("Failed to reinitialize pipeline with new resolution, reverting...");
        config_.ApplyResolution(old_resolution);
        if (state_.mode == PipelineMode::PureIPC) {
            InitParallelPipeline();
        } else {
            InitSerialPipeline();
        }
        return -1;
    }
    
    // 5. 重新注册流消费者
    ReregisterConsumers();
    
    // 6. 如果之前在运行，重新启动
    if (was_streaming) {
        state_.streaming = false;
        if (state_.mode == PipelineMode::PureIPC) {
            rkvideo_start_streaming();
        } else {
            if (serial_pipeline_) {
                serial_pipeline_->Start(frame_process_callback_);
            }
        }
        state_.streaming = true;
    }
    
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    LOG_INFO("<<< Resolution switch completed in {}ms ({} @ {}fps)",
             duration.count(), ResolutionPresetToString(preset), config_.frame_rate);
    
    return 0;
}

int PipelineManager::SetFrameRate(int fps) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!state_.initialized) {
        LOG_ERROR("Pipeline manager not initialized");
        return -1;
    }
    
    // 限制帧率范围
    fps = std::max(1, std::min(fps, 30));
    
    if (fps == config_.frame_rate) {
        LOG_DEBUG("Already at {}fps, skip switch", fps);
        return 0;
    }
    
    int old_fps = config_.frame_rate;
    
    LOG_INFO(">>> Frame rate switch: {} -> {} (cold start)", old_fps, fps);
    
    auto start_time = std::chrono::steady_clock::now();
    
    // 冷切换流程（与分辨率切换相同）
    bool was_streaming = state_.streaming;
    if (was_streaming) {
        Stop();
    }
    
    if (state_.mode == PipelineMode::PureIPC) {
        DeinitParallelPipeline();
    } else {
        DeinitSerialPipeline();
    }
    
    config_.frame_rate = fps;
    
    int ret = 0;
    if (state_.mode == PipelineMode::PureIPC) {
        ret = InitParallelPipeline();
    } else {
        ret = InitSerialPipeline();
    }
    
    if (ret != 0) {
        LOG_ERROR("Failed to reinitialize pipeline with new frame rate, reverting...");
        config_.frame_rate = old_fps;
        if (state_.mode == PipelineMode::PureIPC) {
            InitParallelPipeline();
        } else {
            InitSerialPipeline();
        }
        return -1;
    }
    
    ReregisterConsumers();
    
    if (was_streaming) {
        state_.streaming = false;
        if (state_.mode == PipelineMode::PureIPC) {
            rkvideo_start_streaming();
        } else {
            if (serial_pipeline_) {
                serial_pipeline_->Start(frame_process_callback_);
            }
        }
        state_.streaming = true;
    }
    
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    LOG_INFO("<<< Frame rate switch completed in {}ms", duration.count());
    
    return 0;
}

// ============================================================================
// 流消费者管理
// ============================================================================

void PipelineManager::RegisterStreamConsumer(const std::string& name,
                                              std::function<void(EncodedStreamPtr)> callback,
                                              ConsumerType type,
                                              int queue_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 保存到列表
    StreamConsumerInfo info;
    info.name = name;
    info.callback = callback;
    info.type = type;
    info.queue_size = queue_size;
    consumers_.push_back(info);
    
    // 立即注册到当前管道
    if (state_.mode == PipelineMode::PureIPC) {
        // 并行模式使用 rkvideo 的接口
        rkvideo_register_stream_consumer(name.c_str(),
            [](EncodedStreamPtr stream, void* user_data) {
                auto* cb = static_cast<std::function<void(EncodedStreamPtr)>*>(user_data);
                if (*cb) {
                    (*cb)(stream);
                }
            },
            // 需要持久化 callback，使用 new 分配
            new std::function<void(EncodedStreamPtr)>(callback),
            type, queue_size);
    } else {
        if (serial_pipeline_) {
            serial_pipeline_->RegisterStreamConsumer(name.c_str(), callback, type, queue_size);
        }
    }
    
    LOG_DEBUG("Registered stream consumer: {}", name);
}

void PipelineManager::ClearStreamConsumers() {
    std::lock_guard<std::mutex> lock(mutex_);
    consumers_.clear();
}

void PipelineManager::ReregisterConsumers() {
    LOG_DEBUG("Re-registering {} stream consumers...", consumers_.size());
    
    for (const auto& info : consumers_) {
        if (state_.mode == PipelineMode::PureIPC) {
            rkvideo_register_stream_consumer(info.name.c_str(),
                [](EncodedStreamPtr stream, void* user_data) {
                    auto* cb = static_cast<std::function<void(EncodedStreamPtr)>*>(user_data);
                    if (*cb) {
                        (*cb)(stream);
                    }
                },
                new std::function<void(EncodedStreamPtr)>(info.callback),
                info.type, info.queue_size);
        } else {
            if (serial_pipeline_) {
                serial_pipeline_->RegisterStreamConsumer(info.name.c_str(), 
                                                          info.callback, 
                                                          info.type, 
                                                          info.queue_size);
            }
        }
    }
}

void PipelineManager::SetModeSwitchCallback(ModeSwitchCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    mode_switch_callback_ = std::move(callback);
}

// ============================================================================
// 并行模式管道管理
// ============================================================================

int PipelineManager::InitParallelPipeline() {
    LOG_DEBUG("Initializing parallel pipeline (PureIPC mode)...");
    
    // 使用现有的 rkvideo_init
    int ret = rkvideo_init();
    if (ret != 0) {
        LOG_ERROR("rkvideo_init failed");
        return -1;
    }
    
    return 0;
}

int PipelineManager::DeinitParallelPipeline() {
    LOG_DEBUG("Deinitializing parallel pipeline...");
    return rkvideo_deinit();
}

// ============================================================================
// 串行模式管道管理
// ============================================================================

int PipelineManager::InitSerialPipeline() {
    LOG_DEBUG("Initializing serial pipeline (InferenceAI mode)...");
    
    serial_pipeline_ = std::make_unique<SerialPipeline>();
    
    SerialPipelineConfig serial_config;
    serial_config.width = config_.width;
    serial_config.height = config_.height;
    serial_config.frame_rate = config_.frame_rate;
    serial_config.ai_width = config_.ai_width;
    serial_config.ai_height = config_.ai_height;
    
    int ret = serial_pipeline_->Init(serial_config);
    if (ret != 0) {
        LOG_ERROR("Serial pipeline init failed");
        serial_pipeline_.reset();
        return -1;
    }
    
    return 0;
}

int PipelineManager::DeinitSerialPipeline() {
    LOG_DEBUG("Deinitializing serial pipeline...");
    
    if (serial_pipeline_) {
        serial_pipeline_->Deinit();
        serial_pipeline_.reset();
    }
    
    return 0;
}

}  // namespace rkvideo
