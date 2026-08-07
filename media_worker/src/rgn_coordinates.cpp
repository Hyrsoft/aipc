#include "rgn_coordinates.h"

#include <algorithm>
#include <cmath>

namespace media_worker {
namespace {

int ScaleEdge(int value, int source_size, int target_size) {
    if (source_size <= 0 || target_size <= 0) return value;
    return static_cast<int>(std::llround(
        static_cast<double>(value) * target_size / source_size));
}

}  // namespace

OsdRegion ScaleOsdRegion(const OsdRegion& region, int source_width,
                         int source_height, int target_width,
                         int target_height) {
    const int left = ScaleEdge(region.x, source_width, target_width);
    const int top = ScaleEdge(region.y, source_height, target_height);
    const int right = ScaleEdge(region.x + std::max(0, region.width),
                                source_width, target_width);
    const int bottom = ScaleEdge(region.y + std::max(0, region.height),
                                 source_height, target_height);
    return {left, top, std::max(0, right - left),
            std::max(0, bottom - top)};
}

}  // namespace media_worker
