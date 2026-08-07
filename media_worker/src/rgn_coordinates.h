#pragma once

namespace media_worker {

struct OsdRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

OsdRegion ScaleOsdRegion(const OsdRegion& region, int source_width,
                         int source_height, int target_width,
                         int target_height);

}  // namespace media_worker
