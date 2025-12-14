#pragma once
#include <string>
#include <vector>
#include "Types.h"

namespace aipc::engine {

    class IAIEngine {
    public:
        virtual ~IAIEngine() = default;

        // 初始化：加载模型权重，分配内存
        // 返回 0 表示成功
        virtual int Init(const std::string &model_path) = 0;

        // 推理：输入图片，输出结果列表
        virtual int Inference(const cv::Mat &img, std::vector<ObjectDet> &results) = 0;

        // 获取模型名字 (用于日志)
        virtual std::string GetName() const = 0;
    };

} // namespace aipc::engine
