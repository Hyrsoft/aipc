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
    const auto detection = ai_worker::ObjectFromXywh(
        98, 140, 280, 246, 0.75F, 62, "tv");
    Expect(detection.at("x1") == 98, "x origin is preserved");
    Expect(detection.at("y1") == 140, "y origin is preserved");
    Expect(detection.at("x2") == 378, "VisionG width becomes the right edge");
    Expect(detection.at("y2") == 386, "VisionG height becomes the bottom edge");
    Expect(detection.at("confidence") == 0.75F, "score is preserved");
    Expect(detection.at("class_id") == 62, "class id is preserved");
    Expect(detection.at("label") == "tv", "label is preserved");
    Expect(detection.at("kind") == "object", "object kind is explicit");

    const auto invalid_size = ai_worker::ObjectFromXywh(
        10, 20, -1, -2, 0.5F, 0, "invalid");
    Expect(invalid_size.at("x2") == 10, "negative width does not invert the box");
    Expect(invalid_size.at("y2") == 20, "negative height does not invert the box");
    return 0;
}
