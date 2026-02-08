/**
 * @file ai_model_base.cpp
 * @brief AI 模型基类默认实现
 *
 * @author 好软，好温暖
 * @date 2026-02-07
 */

#include "ai_model_base.h"

#include <sstream>
#include <iomanip>

namespace rknn {

std::string AIModelBase::FormatResultLog(const DetectionResult& result, size_t index,
                                          float letterbox_scale,
                                          int letterbox_pad_x,
                                          int letterbox_pad_y) const {
    // 默认实现：简单的框坐标格式
    // 映射坐标从模型空间到原始图像空间
    int x1 = static_cast<int>((result.box.x - letterbox_pad_x) / letterbox_scale);
    int y1 = static_cast<int>((result.box.y - letterbox_pad_y) / letterbox_scale);
    int x2 = static_cast<int>((result.box.x + result.box.width - letterbox_pad_x) / letterbox_scale);
    int y2 = static_cast<int>((result.box.y + result.box.height - letterbox_pad_y) / letterbox_scale);
    
    std::ostringstream oss;
    oss << "[" << index << "] " << result.label 
        << " @ (" << x1 << "," << y1 << "," << x2 << "," << y2 << ")"
        << " conf=" << std::fixed << std::setprecision(2) << (result.confidence * 100) << "%";
    
    return oss.str();
}

}  // namespace rknn
