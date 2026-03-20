/**
 * @file python_strategy.cpp
 * @brief Python 可编程模型策略实现
 *
 * 使用 pybind11::embed 内嵌 Python 解释器。
 * C++ 完成 NPU 推理后，将 Detection 列表和 ImageBuffer 传给用户 Python 代码做后处理。
 */

#include "python_strategy.h"
#include "common/logger.h"

#include "visiong/core/ImageBuffer.h"
#include "visiong/npu/NPU.h"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <cstdio>

namespace py = pybind11;

namespace media {

// ============================================================================
// 默认 Python 后处理代码
// ============================================================================

const char* PythonStrategy::GetDefaultCode() {
    return R"python(
import visiong

def process(image, detections):
    """
    后处理回调函数。

    Args:
        image: visiong.ImageBuffer (NV12 原始帧)
        detections: list[visiong.Detection] (C++ NPU 推理结果)

    Returns:
        visiong.ImageBuffer (绘制 OSD 后的 BGR 帧)
    """
    bgr = image.to_format("bgr")
    for det in detections:
        x, y, w, h = det.box
        bgr.draw_rectangle(x, y, w, h, color=(0, 255, 0), thickness=3)
        label = f"{det.label} {det.score:.0%}"
        bgr.draw_string(x, y - 8, label, color=(0, 255, 0), scale=1.0, thickness=2)
    return bgr
)python";
}

// ============================================================================
// PythonImpl — 隐藏 pybind11 头文件依赖
// ============================================================================

struct PythonStrategy::PythonImpl {
    std::unique_ptr<py::scoped_interpreter> interpreter;
    py::object process_func;  // 用户的 process(image, detections) 函数
    py::object user_globals;  // 用户代码的全局命名空间
};

// ============================================================================
// 构造与析构
// ============================================================================

PythonStrategy::PythonStrategy(const std::string& model_path,
                               const std::string& label_path,
                               const std::string& model_type_str)
    : model_path_(model_path),
      label_path_(label_path),
      model_type_str_(model_type_str),
      py_impl_(std::make_unique<PythonImpl>()),
      current_code_(GetDefaultCode()) {}

PythonStrategy::~PythonStrategy() {
    Deinit();
}

// ============================================================================
// IModelStrategy 接口
// ============================================================================

std::unique_ptr<NPU> PythonStrategy::CreateNPU() {
    ModelType mt = ParseModelType(model_type_str_);
    auto npu = std::make_unique<NPU>(mt, model_path_, label_path_, 0.25f, 0.45f);
    if (!npu->is_initialized()) {
        LOG_ERROR("Python strategy NPU init failed: model={}, type={}",
                  model_path_, model_type_str_);
        return nullptr;
    }
    LOG_INFO("Python strategy NPU initialized: model={}, type={}",
             model_path_, model_type_str_);
    return npu;
}

bool PythonStrategy::Init() {
    if (!InitPython()) {
        LOG_ERROR("Python interpreter init failed");
        return false;
    }

    // 加载默认代码
    auto err = CompileUserCode(current_code_);
    if (!err.empty()) {
        LOG_ERROR("Failed to load default Python code: {}", err);
        return false;
    }

    LOG_INFO("PythonStrategy initialized");
    return true;
}

void PythonStrategy::Deinit() {
    DeinitPython();
    LOG_INFO("PythonStrategy deinitialized");
}

ImageBuffer PythonStrategy::ProcessFrame(const ImageBuffer& frame, NPU* npu) {
    // 1. C++ 侧完成推理
    std::vector<Detection> detections;
    if (npu) {
        detections = npu->inference(frame);
    }

    // 2. 尝试调用 Python 后处理
    {
        std::lock_guard<std::mutex> lock(code_mutex_);

        if (python_initialized_ && py_impl_->process_func) {
            try {
                py::gil_scoped_acquire acquire;
                py::object result = py_impl_->process_func(frame, detections);

                // 尝试将返回值转为 ImageBuffer
                if (py::isinstance<ImageBuffer>(result)) {
                    return result.cast<ImageBuffer>();
                }

                // 如果返回 None 或无效类型，走 fallback
                last_error_ = "process() did not return an ImageBuffer";
            } catch (const py::error_already_set& e) {
                last_error_ = e.what();
                LOG_WARN("Python process() error: {}", last_error_);
            } catch (const std::exception& e) {
                last_error_ = e.what();
                LOG_WARN("Python process() exception: {}", last_error_);
            }
        }
    }

    // 3. Fallback: 默认绘制（与 YOLOv5 策略相同）
    ImageBuffer draw_frame = frame.get_bgr_version().copy();
    for (const auto& det : detections) {
        auto [x, y, w, h] = det.box;
        draw_frame.draw_rectangle(x, y, w, h, {0, 255, 0}, 3, false);

        char text[64];
        snprintf(text, sizeof(text), "%s %.1f%%", det.label.c_str(), det.score * 100.0f);
        draw_frame.draw_string(x, y - 8, text, {0, 255, 0}, 1.0, 2);
    }
    return draw_frame;
}

// ============================================================================
// Python 编辑器专用接口
// ============================================================================

std::string PythonStrategy::UpdateCode(const std::string& code) {
    std::lock_guard<std::mutex> lock(code_mutex_);

    auto err = CompileUserCode(code);
    if (err.empty()) {
        current_code_ = code;
        last_error_.clear();
        LOG_INFO("Python code updated successfully ({} bytes)", code.size());
    } else {
        last_error_ = err;
        LOG_WARN("Python code update failed: {}", err);
    }
    return err;
}

std::string PythonStrategy::GetCurrentCode() const {
    std::lock_guard<std::mutex> lock(code_mutex_);
    return current_code_;
}

std::string PythonStrategy::GetLastError() const {
    std::lock_guard<std::mutex> lock(code_mutex_);
    return last_error_;
}

std::unique_ptr<NPU> PythonStrategy::UpdateModel(const std::string& model_path,
                                                   const std::string& label_path,
                                                   const std::string& model_type_str) {
    ModelType mt = ParseModelType(model_type_str);
    auto npu = std::make_unique<NPU>(mt, model_path, label_path, 0.25f, 0.45f);
    if (!npu->is_initialized()) {
        LOG_ERROR("UpdateModel failed: model={}, type={}", model_path, model_type_str);
        return nullptr;
    }

    // 更新内部记录
    model_path_ = model_path;
    label_path_ = label_path;
    model_type_str_ = model_type_str;

    LOG_INFO("Model updated: {} ({})", model_path, model_type_str);
    return npu;
}

PythonStrategy::ModelInfo PythonStrategy::GetModelInfo() const {
    return {model_path_, label_path_, model_type_str_};
}

// ============================================================================
// 内部实现
// ============================================================================

ModelType PythonStrategy::ParseModelType(const std::string& type_str) {
    if (type_str == "YOLOV5")      return ModelType::YOLOV5;
    if (type_str == "RETINAFACE")  return ModelType::RETINAFACE;
    if (type_str == "YOLO11")      return ModelType::YOLO11;
    if (type_str == "YOLO11_SEG")  return ModelType::YOLO11_SEG;
    if (type_str == "YOLO11_POSE") return ModelType::YOLO11_POSE;
    if (type_str == "LPRNET")      return ModelType::LPRNET;
    // 默认 YOLOV5
    LOG_WARN("Unknown model type '{}', defaulting to YOLOV5", type_str);
    return ModelType::YOLOV5;
}

bool PythonStrategy::InitPython() {
    if (python_initialized_) {
        return true;
    }

    try {
        py_impl_->interpreter = std::make_unique<py::scoped_interpreter>();

        // 将 visiong Python 包路径加入 sys.path
        py::module_ sys = py::module_::import("sys");
        py::list path = sys.attr("path");
        // 预编译包部署路径（安装后的路径）
        path.attr("insert")(0, "../python");

        python_initialized_ = true;
        LOG_INFO("Python interpreter initialized");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Python interpreter init failed: {}", e.what());
        return false;
    }
}

void PythonStrategy::DeinitPython() {
    if (!python_initialized_) {
        return;
    }

    try {
        py::gil_scoped_acquire acquire;
        py_impl_->process_func = py::none();
        py_impl_->user_globals = py::none();
    } catch (...) {
        // 忽略析构期间的 Python 错误
    }

    py_impl_->interpreter.reset();
    python_initialized_ = false;
}

std::string PythonStrategy::CompileUserCode(const std::string& code) {
    if (!python_initialized_) {
        return "Python interpreter not initialized";
    }

    try {
        py::gil_scoped_acquire acquire;

        // 创建新的命名空间执行用户代码
        py::dict globals = py::module_::import("__main__").attr("__dict__");
        py::dict user_ns;

        // 将 visiong 模块预导入到用户命名空间
        try {
            user_ns["visiong"] = py::module_::import("visiong");
        } catch (const py::error_already_set&) {
            // visiong 不可用时也允许编译（用户可能不用 visiong API）
            LOG_WARN("visiong Python module not available, continuing without it");
        }

        // 执行用户代码
        py::exec(code, user_ns);

        // 检查是否定义了 process 函数
        if (!user_ns.contains("process")) {
            return "Code must define a 'process(image, detections)' function";
        }

        py::object func = user_ns["process"];
        if (!py::isinstance<py::function>(func)) {
            return "'process' must be a function";
        }

        // 成功：更新函数引用
        py_impl_->process_func = func;
        py_impl_->user_globals = user_ns;

        return "";  // 成功
    } catch (const py::error_already_set& e) {
        return std::string("Python error: ") + e.what();
    } catch (const std::exception& e) {
        return std::string("Error: ") + e.what();
    }
}

} // namespace media
