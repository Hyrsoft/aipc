#include "ai_frame_ipc_publisher.h"

#include <cstdlib>
#include <iostream>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
    media_worker::RawAiFrame frame;
    frame.data = {1, 2, 3, 4};
    frame.pts = 42;
    frame.sequence = 7;
    frame.width = 640;
    frame.height = 360;
    frame.y_stride = 640;
    frame.uv_stride = 640;
    frame.main_width = 1920;
    frame.main_height = 1080;
    frame.fit_mode = media_worker::AiFitMode::kContain;
    frame.transform.crop_width = 1920;
    frame.transform.crop_height = 1080;
    const auto encoded = media_worker::EncodeAiFrameIpcFrame(frame);
    Expect(encoded.size() == media_worker::kAiFrameIpcHeaderSize + 4, "encoded length");
    Expect(encoded[0] == 'A' && encoded[3] == 'F', "AIPF magic");
    Expect(encoded[4] == 0 && encoded[5] == 1, "AIPF version");
    Expect(encoded[6] == 0 && encoded[7] == 1, "contain fit mode");
    Expect(encoded[27] == 7, "sequence encoded");
    Expect(media_worker::ParseAiFitMode("cover") == media_worker::AiFitMode::kCover,
           "cover parsed");
    if (failures) return EXIT_FAILURE;
    std::cout << "all AI frame IPC tests passed\n";
    return EXIT_SUCCESS;
}
