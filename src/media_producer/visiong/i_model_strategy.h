/**
 * @file i_model_strategy.h
 * @brief AI 模型策略接口
 *
 * 定义 VisionG 模式下不同 AI 模型的差异化逻辑：
 * - 初始化推理引擎（NPU 或 PPOCR 等）
 * - 推理 + OSD 绘制
 *
 * 公共部分（Camera、VencManager、流分发）由 VisionGProducer 管理，
 * 模型特定逻辑通过此策略接口注入。
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "visiong/npu/NPU.h"

class ImageBuffer;

namespace media {

/**
 * @brief AI 模型策略接口
 *
 * 每种模型（YOLOv5、RetinaFace、PPOCR 等）实现此接口，
 * 封装各自的推理引擎初始化和帧处理（推理 + OSD）逻辑。
 */
class IModelStrategy {
public:
    virtual ~IModelStrategy() = default;

    /**
     * @brief 创建并初始化 NPU 实例（使用 NPU 类的模型需实现）
     * @return 初始化好的 NPU 指针；不使用 NPU 的策略返回 nullptr
     */
    virtual std::unique_ptr<NPU> CreateNPU() { return nullptr; }

    /**
     * @brief 初始化策略自身的推理资源（不使用 NPU 的模型在此初始化）
     * @return true 成功
     */
    virtual bool Init() { return true; }

    /** @brief 释放策略自身的推理资源 */
    virtual void Deinit() {}

    /**
     * @brief 对一帧执行推理并绘制 OSD
     *
     * @param frame 原始帧（NV12，来自 Camera::snapshot()）
     * @param npu 已初始化的 NPU 实例（可能为 nullptr）
     * @return 绘制 OSD 后的帧（BGR），供 VencManager 编码
     */
    virtual ImageBuffer ProcessFrame(const ImageBuffer& frame, NPU* npu) = 0;

    /** @brief 获取策略/模型名称（用于日志） */
    virtual const char* GetName() const = 0;

protected:
    IModelStrategy() = default;
};

} // namespace media
