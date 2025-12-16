#pragma once

#include <vector>

#include <opencv2/core.hpp>

#include "ai/Types.h"

namespace aipc::osd {

    void draw_detections(cv::Mat &bgr, const std::vector<aipc::ai::ObjectDet> &dets);

} // namespace aipc::osd
