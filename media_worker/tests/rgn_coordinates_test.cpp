#include <cstdlib>
#include <iostream>

#include "rgn_coordinates.h"

namespace {

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

}  // namespace

int main() {
    const media_worker::OsdRegion main_region{1764, 81, 153, 399};
    const auto vi_region = media_worker::ScaleOsdRegion(
        main_region, 1920, 1080, 2304, 1296);
    Expect(vi_region.x == 2117, "VI left edge uses sensor coordinates");
    Expect(vi_region.y == 97, "VI top edge uses sensor coordinates");
    Expect(vi_region.width == 183, "VI width preserves normalized extent");
    Expect(vi_region.height == 479, "VI height preserves normalized extent");

    const auto unchanged = media_worker::ScaleOsdRegion(
        main_region, 1920, 1080, 1920, 1080);
    Expect(unchanged.x == main_region.x, "same-size x remains unchanged");
    Expect(unchanged.y == main_region.y, "same-size y remains unchanged");
    Expect(unchanged.width == main_region.width,
           "same-size width remains unchanged");
    Expect(unchanged.height == main_region.height,
           "same-size height remains unchanged");
    return 0;
}
