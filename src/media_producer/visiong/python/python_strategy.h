/**
 * @file python_strategy.h
 * @brief Python 可编程模型策略
 *
 * 通过 pybind11::embed 内嵌 Python 解释器，允许用户通过 Web 编辑器
 * 编写 Python 后处理代码（拿到推理结果后决定 OSD 绘制逻辑）。
 *
 * 工作流程：
 * 1. C++ 侧完成 NPU 推理，获取 Detection 列表
 * 2. 将 ImageBuffer + Detection 列表传给用户 Python 回调
 * 3. Python 代码决定如何绘制 OSD，返回处理后的 ImageBuffer
 *
 * NPU 模型可通过 UpdateModel() 动态切换（需暂停帧循环）。
 * Python 代码可通过 UpdateCode() 热更新。
 */

#pragma once

#include "../i_model_strategy.h"

#include <mutex>
#include <string>
#include <vector>

namespace media {

/**
 * @brief C++ 注册的可用模型类型
 *
 * 限制用户只能选择 visiong 库支持的模型类型。
 * 前端通过 /api/models/registered 获取此列表。
 */
struct RegisteredModel {
    std::string name;           ///< 显示名称（如 "YOLOv5 物体检测"）
    std::string type_str;       ///< 模型类型字符串（如 "YOLOV5"）
    std::string default_model;  ///< 默认模型文件名（如 "yolov5.rknn"）
    std::string default_labels; ///< 默认标签文件名（空字符串表示无标签）
    std::string description;    ///< 描述
};

/**
 * @brief Python 可编程模型策略
 *
 * 默认使用 YOLOv5 模型，支持动态切换模型和热更新 Python 代码。
 */
class PythonStrategy : public IModelStrategy {
public:
    /**
     * @brief 构造函数
     * @param model_path 初始 RKNN 模型文件路径（如 "../model/yolov5.rknn"）
     * @param label_path 标签文件路径（如 "../model/coco_80_labels_list.txt"）
     * @param model_type_str 模型类型字符串（如 "YOLOV5"）
     */
    PythonStrategy(const std::string& model_path,
                   const std::string& label_path,
                   const std::string& model_type_str = "YOLOV5");

    ~PythonStrategy() override;

    // ========== IModelStrategy 接口 ==========

    std::unique_ptr<NPU> CreateNPU() override;
    bool Init() override;
    void Deinit() override;
    ImageBuffer ProcessFrame(const ImageBuffer& frame, NPU* npu) override;
    const char* GetName() const override { return "Python"; }

    // ========== 注册模型列表 ==========

    /**
     * @brief 获取 C++ 注册的可用模型类型列表
     *
     * 硬编码 visiong 库支持的 ModelType 枚举值。
     * 前端使用此列表渲染模型类型选择器，防止用户选择不支持的模型。
     */
    static const std::vector<RegisteredModel>& GetRegisteredModels();

    // ========== Python 编辑器专用接口 ==========

    /**
     * @brief 热更新用户 Python 代码
     * @param code Python 源代码（须包含 def process(image, detections) 函数）
     * @return 空字符串表示成功，否则返回错误信息
     */
    std::string UpdateCode(const std::string& code);

    /**
     * @brief 获取当前 Python 代码
     */
    std::string GetCurrentCode() const;

    /**
     * @brief 获取最近一次 Python 执行错误
     */
    std::string GetLastError() const;

    /**
     * @brief 更新 NPU 模型（需要外部先暂停帧循环）
     * @param model_path RKNN 模型文件路径
     * @param label_path 标签文件路径
     * @param model_type_str 模型类型字符串
     * @return 成功创建的 NPU 指针，失败返回 nullptr
     *
     * @note 调用者需要在帧循环暂停后调用此方法，
     *       并用返回值替换 VisionGProducer 中的 npu_ 成员。
     */
    std::unique_ptr<NPU> UpdateModel(const std::string& model_path,
                                     const std::string& label_path,
                                     const std::string& model_type_str);

    /**
     * @brief 获取当前模型信息
     */
    struct ModelInfo {
        std::string model_path;
        std::string label_path;
        std::string model_type;
    };
    ModelInfo GetModelInfo() const;

private:
    /** @brief 解析模型类型字符串到 ModelType 枚举 */
    static ModelType ParseModelType(const std::string& type_str);

    /** @brief 默认的后处理 Python 代码 */
    static const char* GetDefaultCode();

    /** @brief 初始化 Python 解释器 */
    bool InitPython();

    /** @brief 关闭 Python 解释器 */
    void DeinitPython();

    /** @brief 编译用户代码并提取 process 函数 */
    std::string CompileUserCode(const std::string& code);

private:
    // 模型信息
    std::string model_path_;
    std::string label_path_;
    std::string model_type_str_;

    // Python 状态（通过 Impl 隐藏 pybind11 依赖）
    struct PythonImpl;
    std::unique_ptr<PythonImpl> py_impl_;

    // 用户代码
    mutable std::mutex code_mutex_;
    std::string current_code_;
    std::string last_error_;
    bool python_initialized_ = false;
};

} // namespace media
