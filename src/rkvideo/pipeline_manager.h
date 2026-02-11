/**
 * @file pipeline_manager.h
 * @brief 管道模式管理器 - 统一管理双模切换
 *
 * 提供统一的接口来管理两种工作模式：
 * - Mode A: Pure IPC (Parallel) - 零拷贝硬件绑定
 * - Mode B: Inference AI (Serial) - 软件控制串行流水线
 *
 * 冷切换：
 * 模式切换时需要完全重建 MPI 管道，因此有一个短暂的停流时间。
 * 
 * @author 好软，好温暖
 * @date 2026-02-11
 */

#pragma once

#include "pipeline_mode.h"
#include "serial_pipeline.h"
#include "stream_dispatcher.h"

#include <mutex>
#include <atomic>
#include <memory>
#include <functional>
#include <vector>

namespace rkvideo {

// ============================================================================
// 管道管理器配置
// ============================================================================

/**
 * @brief 管道管理器配置
 */
struct PipelineManagerConfig {
    ResolutionPreset resolution = ResolutionPreset::R_1080P;  ///< 分辨率预设
    int width = 1920;               ///< 视频宽度（由 resolution 决定）
    int height = 1080;              ///< 视频高度（由 resolution 决定）
    int frame_rate = 30;            ///< 帧率（最高 30）
    int ai_width = 640;             ///< AI 处理分辨率宽度
    int ai_height = 640;            ///< AI 处理分辨率高度
    
    PipelineMode initial_mode = PipelineMode::PureIPC;  ///< 初始模式
    
    /**
     * @brief 应用分辨率预设
     */
    void ApplyResolution(ResolutionPreset preset) {
        resolution = preset;
        auto res = GetResolutionConfig(preset);
        width = res.width;
        height = res.height;
        frame_rate = std::min(frame_rate, res.framerate);
    }
};

// ============================================================================
// 流消费者注册信息
// ============================================================================

/**
 * @brief 流消费者注册信息
 */
struct StreamConsumerInfo {
    std::string name;
    std::function<void(EncodedStreamPtr)> callback;
    ConsumerType type = ConsumerType::AsyncIO;
    int queue_size = 3;
};

// ============================================================================
// 管道管理器
// ============================================================================

/**
 * @class PipelineManager
 * @brief 管道模式管理器
 *
 * 管理视频管道的生命周期和模式切换。
 * 
 * 使用方式：
 * @code
 * PipelineManager mgr;
 * mgr.Init(config);
 * 
 * // 注册流消费者（模式切换时会自动重新注册）
 * mgr.RegisterStreamConsumer("rtsp", rtsp_callback);
 * 
 * // 启动纯 IPC 模式
 * mgr.Start();
 * 
 * // 切换到 AI 推理模式
 * mgr.SwitchMode(PipelineMode::InferenceAI, ai_process_callback);
 * 
 * // 切换回纯 IPC 模式
 * mgr.SwitchMode(PipelineMode::PureIPC, nullptr);
 * @endcode
 */
class PipelineManager {
public:
    /**
     * @brief 获取单例实例
     */
    static PipelineManager& Instance();

    // 禁止拷贝
    PipelineManager(const PipelineManager&) = delete;
    PipelineManager& operator=(const PipelineManager&) = delete;

    /**
     * @brief 初始化管道管理器
     * 
     * @param config 配置参数
     * @return 0 成功，-1 失败
     */
    int Init(const PipelineManagerConfig& config);

    /**
     * @brief 反初始化管道管理器
     * @return 0 成功
     */
    int Deinit();

    /**
     * @brief 启动当前模式的管道
     * @return true 成功
     */
    bool Start();

    /**
     * @brief 停止当前管道
     */
    void Stop();

    /**
     * @brief 切换管道模式（冷切换）
     * 
     * 切换过程：
     * 1. 停止当前管道
     * 2. 反初始化当前 MPI 配置
     * 3. 按新模式重新初始化 MPI
     * 4. 重新注册流消费者
     * 5. 启动新管道
     * 
     * @param mode 目标模式
     * @param process_callback 帧处理回调（仅 InferenceAI 模式需要）
     * @return 0 成功，-1 失败
     * 
     * @note 切换过程中会有短暂的停流时间（冷启动）
     */
    int SwitchMode(PipelineMode mode, FrameProcessCallback process_callback = nullptr);

    /**
     * @brief 获取当前模式
     */
    PipelineMode GetCurrentMode() const { return state_.mode; }

    /**
     * @brief 获取当前状态
     */
    PipelineState GetState() const { return state_; }

    /**
     * @brief 检查是否正在运行
     */
    bool IsRunning() const { return state_.streaming; }

    /**
     * @brief 获取当前分辨率预设
     */
    ResolutionPreset GetCurrentResolution() const { return config_.resolution; }

    /**
     * @brief 设置分辨率（冷切换）
     * 
     * 切换过程：
     * 1. 停止当前管道
     * 2. 反初始化当前 MPI 配置
     * 3. 更新分辨率配置
     * 4. 重新初始化 MPI
     * 5. 重新注册流消费者
     * 6. 启动管道
     * 
     * @param preset 分辨率预设
     * @return 0 成功，-1 失败
     * 
     * @note 推理模式下强制使用 480p，设置其他分辨率无效
     * @note 切换过程中会有短暂的停流时间（冷启动）
     */
    int SetResolution(ResolutionPreset preset);

    /**
     * @brief 设置帧率
     * @param fps 帧率（1-30）
     * @return 0 成功，-1 失败
     */
    int SetFrameRate(int fps);

    /**
     * @brief 注册流消费者
     * 
     * 消费者信息会被保存，模式切换时自动重新注册。
     * 
     * @param name 消费者名称
     * @param callback 回调函数
     * @param type 消费者类型
     * @param queue_size 队列大小
     */
    void RegisterStreamConsumer(const std::string& name,
                                 std::function<void(EncodedStreamPtr)> callback,
                                 ConsumerType type = ConsumerType::AsyncIO,
                                 int queue_size = 3);

    /**
     * @brief 清除所有流消费者
     */
    void ClearStreamConsumers();

    /**
     * @brief 设置模式切换回调
     * 
     * 在模式切换完成后调用，用于通知外部模块。
     */
    using ModeSwitchCallback = std::function<void(PipelineMode oldMode, PipelineMode newMode)>;
    void SetModeSwitchCallback(ModeSwitchCallback callback);

private:
    PipelineManager() = default;
    ~PipelineManager();

    /**
     * @brief 初始化并行模式管道
     */
    int InitParallelPipeline();

    /**
     * @brief 反初始化并行模式管道
     */
    int DeinitParallelPipeline();

    /**
     * @brief 初始化串行模式管道
     */
    int InitSerialPipeline();

    /**
     * @brief 反初始化串行模式管道
     */
    int DeinitSerialPipeline();

    /**
     * @brief 重新注册所有流消费者
     */
    void ReregisterConsumers();

    PipelineManagerConfig config_;
    PipelineState state_;
    
    std::mutex mutex_;
    
    // 串行管道实例
    std::unique_ptr<SerialPipeline> serial_pipeline_;
    
    // 用于串行模式的帧处理回调
    FrameProcessCallback frame_process_callback_;
    
    // 保存的流消费者列表（用于模式切换后重新注册）
    std::vector<StreamConsumerInfo> consumers_;
    
    // 模式切换回调
    ModeSwitchCallback mode_switch_callback_;
};

}  // namespace rkvideo
