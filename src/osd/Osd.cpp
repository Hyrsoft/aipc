#include "Osd.hpp"

#include <cstdio>

#include <opencv2/imgproc.hpp>

namespace aipc::osd {

void DrawDetections(cv::Mat &bgr, const std::vector<aipc::ai::ObjectDet> &dets) {
    for (const auto &det : dets) {
        cv::rectangle(bgr, det.box, cv::Scalar(0, 255, 0), 3);

        char text[64];
        std::snprintf(text, sizeof(text), "%s %.1f%%", det.label.c_str(), det.score * 100);
        cv::putText(bgr, text, cv::Point(det.box.x, det.box.y - 8), cv::FONT_HERSHEY_SIMPLEX, 1, cv::Scalar(0, 255, 0), 2);

        for (const auto &pt : det.landmarks) {
            cv::circle(bgr, pt, 2, cv::Scalar(0, 0, 255), -1);
        }
    }
}

} // namespace aipc::osd
