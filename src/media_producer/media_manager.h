/**
 * @file media_manager.h
 * @brief 媒体管理器 - 生命周期管理者和模式切换控制器
 *
 * 设计模式：策略模式上下文 (Strategy Context) + 控制器 (Controller)
 *
 * 核心职责：
 * 1. 持有基类指针 (unique_ptr<IMediaProducer>)
 * 2. 管理生产者的生命周期
 * 3. 实现"冷切换"逻辑：销毁旧实例 -> 创建新实例 -> 重新连线
 * 4. 保存流消费者注册信息，模式切换时自动重新注册
 *
 * 冷切换流程：
 * 1. 停止当前生产者 (Stop)
 * 2. 销毁当前生产者 (触发析构函数，释放硬件资源)
 * 3. 创建新生产者 (根据目标模式)
 * 4. 初始化新生产者 (分配硬件资源)
 * 5. 重新注册流消费者
 * 6. 启动新生产者 (Start)
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#pragma once

#include "i_media_producer.h"

#include <memory>
#include <vector>
#include <mutex>
#include <functional>
#include <string>

namespace media {

/**
 * @brief 流消费者注册信息
 */
struct StreamConsumerRegistration {
    std::string name;
    StreamCallback callback;
    StreamConsumerType type = StreamConsumerType::AsyncIO;
    int queue_size = 3;
};

/**
 * @brief 模式切换回调
 */
using ModeSwitchCallback = std::function<void(ProducerMode old_mode, ProducerMode new_mode)>;

/**
 * @class MediaManager
 * @brief 媒体管理器（单例）
 *
 * 统一管理视频生产者的生命周期和模式切换。
 * 对外提供简洁的接口，隐藏内部的硬件复杂性。
 *
 * 使用方式：
 * @code
 * auto& mgr = MediaManager::Instance();
 * 
 * // 配置并初始化
 * ProducerConfig config;
 * config.resolution = Resolution::R_1080P;
 * mgr.Init(ProducerMode::SimpleIPC, config);
 * 
 * // 注册流消费者（切换模式时自动重新注册）
 * mgr.RegisterStreamConsumer("rtsp", rtsp_callback);
 * mgr.RegisterStreamConsumer("webrtc", webrtc_callback);
 * 
 * // 启动
 * mgr.Start();
 * 
 * // 切换到 YOLOv5 模式（冷切换）
 * mgr.SwitchMode(ProducerMode::YoloV5);
 * 
 * // 切回纯 IPC 模式
 * mgr.SwitchMode(ProducerMode::SimpleIPC);
 * 
 * // 停止
 * mgr.Stop();
 * mgr.Deinit();
 * @endcode
 */
class MediaManager {
public:
    /**
     * @brief 获取单例实例
     */
    static MediaManager& Instance();

    // 禁止拷贝
    MediaManager(const MediaManager&) = delete;
    MediaManager& operator=(const MediaManager&) = delete;

    // ========== 生命周期管理 ==========

    /**
     * @brief 初始化媒体管理器
     * 
     * @param mode 初始模式
     * @param config 配置参数
     * @return 0 成功，-1 失败
     */
    int Init(ProducerMode mode, const ProducerConfig& config);

    /**
     * @brief 反初始化
     * @return 0 成功
     */
    int Deinit();

    /**
     * @brief 启动媒体流
     * @return true 成功
     */
    bool Start();

    /**
     * @brief 停止媒体流
     */
    void Stop();

    // ========== 模式切换 ==========

    /**
     * @brief 切换生产者模式（冷切换）
     * 
     * 执行完整的"销毁-创建-连线"流程。
     * 
     * @param mode 目标模式
     * @return 0 成功，-1 失败
     * 
     * @note 切换过程中会有短暂的停流时间
     */
    int SwitchMode(ProducerMode mode);

    /**
     * @brief 获取当前模式
     */
    ProducerMode GetCurrentMode() const { return current_mode_; }

    /**
     * @brief 检查是否已初始化
     */
    bool IsInitialized() const { return initialized_; }

    /**
     * @brief 检查是否正在运行
     */
    bool IsRunning() const;

    // ========== 配置接口 ==========

    /**
     * @brief 设置分辨率（需要重新初始化）
     * @param preset 分辨率预设
     * @return 0 成功，-1 失败
     */
    int SetResolution(Resolution preset);

    /**
     * @brief 设置帧率
     * @param fps 帧率
     * @return 0 成功，-1 失败
     */
    int SetFrameRate(int fps);

    /**
     * @brief 获取当前配置
     */
    const ProducerConfig& GetConfig() const { return config_; }

    // ========== 流消费者管理 ==========

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
    void RegisterStreamConsumer(
        const std::string& name,
        StreamCallback callback,
        StreamConsumerType type = StreamConsumerType::AsyncIO,
        int queue_size = 3);

    /**
     * @brief 清除所有流消费者
     */
    void ClearStreamConsumers();

    // ========== 回调设置 ==========

    /**
     * @brief 设置模式切换回调
     */
    void SetModeSwitchCallback(ModeSwitchCallback callback);

    // ========== 状态查询 ==========

    /**
     * @brief 获取当前生产者类型名称
     */
    const char* GetCurrentTypeName() const;

    /**
     * @brief 获取模式切换次数
     */
    uint64_t GetModeSwitchCount() const { return mode_switch_count_; }

private:
    MediaManager() = default;
    ~MediaManager();

    /**
     * @brief 创建生产者实例
     */
    std::unique_ptr<IMediaProducer> CreateProducerInstance(ProducerMode mode);

    /**
     * @brief 重新注册所有流消费者
     */
    void ReregisterConsumers();

private:
    std::mutex mutex_;
    
    bool initialized_ = false;
    ProducerMode current_mode_ = ProducerMode::SimpleIPC;
    ProducerConfig config_;
    
    // 当前生产者实例
    std::unique_ptr<IMediaProducer> producer_;
    
    // 保存的流消费者列表（用于模式切换后重新注册）
    std::vector<StreamConsumerRegistration> consumers_;
    
    // 回调
    ModeSwitchCallback mode_switch_callback_;
    
    // 统计
    uint64_t mode_switch_count_ = 0;
};

}  // namespace media
