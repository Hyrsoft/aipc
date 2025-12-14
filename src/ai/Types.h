#pragma once

#include <opencv2/core.hpp>

#include <string>
#include <vector>

namespace aipc::ai {

    struct ObjectDet {
        cv::Rect box; // 检测框 (基于原图坐标)
        float score; // 置信度
        int class_id; // 类别ID
        std::string label; // 类别名称 (person, face...)
        std::vector<cv::Point2f> landmarks; // 关键点 (RetinaFace专用, YOLO为空即可)
    };

} // namespace aipc::ai
