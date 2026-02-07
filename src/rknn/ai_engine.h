/**
 * @file ai_engine.h
 * @brief AI 推理引擎 - 线程安全的模型管理与调度
 *
 * 提供核心功能：
 * - 线程安全的模型动态切换（冷切换策略）
 * - 统一的推理接口
 * - 模型生命周期管理
 * - VPSS 分辨率适配支持
 *
 * 设计原则：
 * - 基于策略模式，支持运行时切换算法
 * - 互斥锁保护，确保切换和推理的线程安全
 * - RAII 管理资源，自动释放旧模型
 *
 * @author 好软，好温暖
 * @date 2026-02-05
 */

#pragma once

#include <memory>
#include <mutex>
#include <atomic>
#include <functional>

#include "ai_model_base.h"
#include "ai_types.h"

namespace rknn {

/**
 * @brief VPSS 重配置回调函数类型
 *
 * 当模型切换导致输入尺寸变化时调用，用于重配置 VPSS 通道
 * @param width 新的输入宽度
 * @param height 新的输入高度
 * @return 0 成功，负值失败
 */
using VpssReconfigureCallback = std::function<int(int width, int height)>;

/**
 * @class AIEngine
 * @brief AI 推理引擎单例类
 *
 * 管理 AI 模型的加载、切换和推理，提供线程安全的接口。
 * 采用"冷切换"策略：Stop -> Unload -> Load -> Start
 */
class AIEngine {
public:
    /**
     * @brief 获取单例实例
     */
    static AIEngine& Instance();

    // 禁止拷贝和移动
    AIEngine(const AIEngine&) = delete;
    AIEngine& operator=(const AIEngine&) = delete;

    // ========================================================================
    // 模型管理
    // ========================================================================

    /**
     * @brief 切换模型（线程安全）
     *
     * 实现冷切换策略：
     * 1. 获取互斥锁
     * 2. 销毁旧模型（RAII 自动释放资源）
     * 3. 创建并初始化新模型
     * 4. 如果尺寸变化，回调 VPSS 重配置
     *
     * @param type 目标模型类型
     * @param config 模型配置
     * @return 0 成功，负值失败
     *
     * @note 切换过程中推理请求会被阻塞
     * @note 切换失败时保持无模型状态
     */
    int SwitchModel(ModelType type, const ModelConfig& config);

    /**
     * @brief 切换模型（简化版本）
     *
     * 使用已设置的模型目录自动构建模型路径
     *
     * @param type 目标模型类型
     * @return 0 成功，负值失败
     */
    int SwitchModel(ModelType type);

    /**
     * @brief 卸载当前模型
     *
     * 释放模型资源，引擎进入空闲状态
     */
    void UnloadModel();

    /**
     * @brief 检查是否有模型加载
     */
    bool HasModel() const;

    /**
     * @brief 获取当前模型类型
     */
    ModelType GetCurrentModelType() const;

    /**
     * @brief 获取当前模型信息
     */
    ModelInfo GetModelInfo() const;

    // ========================================================================
    // 推理接口
    // ========================================================================

    /**
     * @brief 处理一帧图像（线程安全）
     *
     * 执行完整的推理流程：SetInput -> Run -> GetResults
     *
     * @param data 输入图像数据（RGB888）
     * @param width 图像宽度
     * @param height 图像高度
     * @param[out] results 检测结果输出
     * @return 0 成功，负值失败
     *
     * @note 如果没有模型加载，返回空结果
     */
    int ProcessFrame(const void* data, int width, int height, DetectionResultList& results);

    /**
     * @brief 直接写入输入内存（零拷贝）
     *
     * 获取输入内存地址，允许外部直接写入数据
     *
     * @param[out] addr 输入内存虚拟地址
     * @param[out] size 输入内存大小
     * @return 0 成功，负值失败
     */
    int GetInputMemory(void** addr, int* size);

    /**
     * @brief 执行推理（零拷贝模式）
     *
     * 数据已通过 GetInputMemory 写入，直接执行推理
     *
     * @param[out] results 检测结果输出
     * @return 0 成功，负值失败
     */
    int RunInference(DetectionResultList& results);

    // ========================================================================
    // 配置接口
    // ========================================================================

    /**
     * @brief 设置 VPSS 重配置回调
     *
     * 当模型输入尺寸变化时，引擎会调用此回调来重配置 VPSS 通道
     *
     * @param callback 回调函数
     */
    void SetVpssReconfigureCallback(VpssReconfigureCallback callback);

    /**
     * @brief 设置模型目录
     *
     * @param dir 模型文件所在目录路径
     */
    void SetModelDir(const std::string& dir);

    /**
     * @brief 获取模型目录
     */
    const std::string& GetModelDir() const;

    /**
     * @brief 获取模型需要的输入尺寸
     *
     * @param[out] width 输入宽度
     * @param[out] height 输入高度
     * @return true 有模型，false 无模型
     */
    bool GetRequiredInputSize(int& width, int& height) const;

    /**
     * @brief 格式化检测结果日志（委托给模型实现）
     *
     * @param result 单个检测结果
     * @param index 结果索引
     * @param letterbox_scale letterbox 缩放比例
     * @param letterbox_pad_x letterbox X 方向填充
     * @param letterbox_pad_y letterbox Y 方向填充
     * @return 格式化的日志字符串
     */
    std::string FormatResultLog(const DetectionResult& result, size_t index,
                                 float letterbox_scale = 1.0f,
                                 int letterbox_pad_x = 0,
                                 int letterbox_pad_y = 0) const;

private:
    AIEngine();
    ~AIEngine();

private:
    /// 当前模型实例
    std::unique_ptr<AIModelBase> model_;
    
    /// 保护模型指针的互斥锁
    mutable std::mutex mutex_;
    
    /// 当前模型类型（用于快速查询，避免加锁）
    std::atomic<ModelType> current_type_{ModelType::kNone};
    
    /// VPSS 重配置回调
    VpssReconfigureCallback vpss_callback_;
    
    /// 上一次的模型输入尺寸（用于检测尺寸变化）
    int last_input_width_ = 0;
    int last_input_height_ = 0;
    
    /// 模型文件目录
    std::string model_dir_;
};

}  // namespace rknn
