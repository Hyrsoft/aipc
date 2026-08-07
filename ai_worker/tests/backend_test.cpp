#include <cstdlib>
#include <iostream>
#include <string>

#include "backend.h"

namespace {

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

}  // namespace

int main() {
    const auto detection = ai_worker::DetectionFromXywh(
        98, 140, 280, 246, 0.75F, 62, "tv");
    Expect(detection.x1 == 98, "x origin is preserved");
    Expect(detection.y1 == 140, "y origin is preserved");
    Expect(detection.x2 == 378, "VisionG width becomes the right edge");
    Expect(detection.y2 == 386, "VisionG height becomes the bottom edge");
    Expect(detection.score == 0.75F, "score is preserved");
    Expect(detection.class_id == 62, "class id is preserved");
    Expect(detection.label == "tv", "label is preserved");

    const auto invalid_size = ai_worker::DetectionFromXywh(
        10, 20, -1, -2, 0.5F, 0, "invalid");
    Expect(invalid_size.x2 == 10, "negative width does not invert the box");
    Expect(invalid_size.y2 == 20, "negative height does not invert the box");
    return 0;
}
