/**
 * @file ai_model_base.h
 * @brief AI 模型抽象基类 - 策略模式接口定义
 *
 * 定义 AI 模型的统一接口，允许在运行时动态切换不同的算法实现。
 * 遵循 Google C++ Style Guide 和 RAII 原则。
 *
 * 设计要点：
 * - 纯虚函数定义接口，子类实现具体算法
 * - 统一的生命周期管理（Init/Deinit）
 * - 零拷贝输入支持（DMA Buffer）
 * - 异步推理支持（可选）
 *
 * @author 好软，好温暖
 * @date 2026-02-05
 */

#pragma once

#include <memory>

#include "ai_types.h"

namespace rknn {

/**
 * @class AIModelBase
 * @brief AI 模型抽象基类
 *
 * 所有 AI 模型实现必须继承此类并实现纯虚函数。
 * 使用策略模式允许运行时切换不同的模型。
 */
class AIModelBase {
public:
    /**
     * @brief 虚析构函数
     * 
     * 确保通过基类指针删除子类对象时正确调用子类析构函数
     */
    virtual ~AIModelBase() = default;

    // ========================================================================
    // 生命周期管理
    // ========================================================================
    
    /**
     * @brief 初始化模型
     *
     * 加载 RKNN 模型文件，初始化 NPU 上下文，分配输入输出内存。
     *
     * @param config 模型配置参数
     * @return 0 成功，负值失败
     *
     * @note 调用 Init 前模型处于未初始化状态
     * @note 重复调用 Init 会先释放旧资源
     */
    virtual int Init(const ModelConfig& config) = 0;

    /**
     * @brief 反初始化模型
     *
     * 释放所有 RKNN 资源，包括输入输出内存和上下文。
     * 析构函数会自动调用此方法。
     */
    virtual void Deinit() = 0;

    /**
     * @brief 检查模型是否已初始化
     * @return true 已初始化，false 未初始化
     */
    virtual bool IsInitialized() const = 0;

    // ========================================================================
    // 推理接口
    // ========================================================================

    /**
     * @brief 设置输入图像数据（拷贝方式）
     *
     * 将图像数据拷贝到 RKNN 输入内存。适用于非 DMA Buffer 的情况。
     *
     * @param data 图像数据指针（RGB888 格式）
     * @param width 图像宽度
     * @param height 图像高度
     * @param stride 行步长（字节），0 表示紧凑排列
     * @return 0 成功，负值失败
     *
     * @note 图像尺寸应与模型输入尺寸匹配，否则内部会进行缩放
     */
    virtual int SetInput(const void* data, int width, int height, int stride = 0) = 0;

    /**
     * @brief 设置输入（零拷贝方式）
     *
     * 直接使用 DMA Buffer 作为输入，避免内存拷贝。
     *
     * @param dma_fd DMA Buffer 文件描述符
     * @param size Buffer 大小
     * @param width 图像宽度
     * @param height 图像高度
     * @return 0 成功，负值失败
     *
     * @note 需要 RKNN 支持零拷贝模式
     */
    virtual int SetInputDma(int dma_fd, int size, int width, int height) = 0;

    /**
     * @brief 执行推理
     *
     * 同步执行 NPU 推理。调用此函数前必须先调用 SetInput。
     *
     * @return 0 成功，负值失败
     */
    virtual int Run() = 0;

    /**
     * @brief 获取推理结果
     *
     * 执行后处理（如 NMS）并返回检测结果。
     * 结果已转换为统一的 DetectionResult 格式。
     *
     * @param results [out] 检测结果输出
     * @return 0 成功，负值失败
     */
    virtual int GetResults(DetectionResultList& results) = 0;

    // ========================================================================
    // 信息查询
    // ========================================================================

    /**
     * @brief 获取模型信息
     * @return 模型信息结构体
     */
    virtual ModelInfo GetModelInfo() const = 0;

    /**
     * @brief 获取模型类型
     * @return 模型类型枚举
     */
    virtual ModelType GetType() const = 0;

    /**
     * @brief 获取输入尺寸
     * @param[out] width 输入宽度
     * @param[out] height 输入高度
     */
    virtual void GetInputSize(int& width, int& height) const = 0;

    /**
     * @brief 获取输入内存虚拟地址
     * 
     * 用于直接向输入内存写入数据（零拷贝）
     *
     * @return 输入内存虚拟地址，nullptr 表示不可用
     */
    virtual void* GetInputVirtAddr() const = 0;

    /**
     * @brief 获取输入内存大小
     * @return 输入内存字节数
     */
    virtual int GetInputMemSize() const = 0;

protected:
    // 禁止直接实例化基类
    AIModelBase() = default;

    // 禁止拷贝和移动（模型资源不应被拷贝）
    AIModelBase(const AIModelBase&) = delete;
    AIModelBase& operator=(const AIModelBase&) = delete;
    AIModelBase(AIModelBase&&) = delete;
    AIModelBase& operator=(AIModelBase&&) = delete;
};

/**
 * @brief 创建 AI 模型实例的工厂函数
 *
 * @param type 模型类型
 * @return 模型实例的智能指针，失败返回 nullptr
 */
std::unique_ptr<AIModelBase> CreateModel(ModelType type);

}  // namespace rknn
