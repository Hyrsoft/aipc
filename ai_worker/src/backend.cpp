#include "backend.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#if AIPC_ENABLE_VISIONG
#include <visiong/core/ImageBuffer.h>
#include <visiong/npu/NPU.h>
#endif

namespace ai_worker {
namespace {

int SaturatingAdd(int value, int delta) {
    const auto result = static_cast<long long>(value) +
                        static_cast<long long>(std::max(0, delta));
    return static_cast<int>(std::min(
        result, static_cast<long long>(std::numeric_limits<int>::max())));
}

class MockBackend final : public Backend {
public:
    std::vector<DetectionResult> Infer(const Frame& frame) override {
        const int width = static_cast<int>(frame.width);
        const int height = static_cast<int>(frame.height);
        const int offset =
            static_cast<int>(frame.sequence % std::max(1, width / 4));
        return {{offset, height / 4, std::min(width - 1, offset + width / 3),
                 std::min(height - 1, height * 3 / 4), 0.9F, 0, "mock"}};
    }
    const char* Name() const override { return "mock"; }
};

#if AIPC_ENABLE_VISIONG
class VisionGBackend final : public Backend {
public:
    VisionGBackend(const fs::path& model, const fs::path& labels,
                   float threshold, float nms_threshold)
        : npu_(ModelType::YOLOV5, model.string(), labels.string(), threshold,
               nms_threshold) {
        if (!npu_.is_initialized()) {
            throw std::runtime_error("VisionG NPU initialization failed");
        }
    }

    std::vector<DetectionResult> Infer(const Frame& frame) override {
        ImageBuffer image(static_cast<int>(frame.width),
                          static_cast<int>(frame.height), RK_FMT_YUV420SP,
                          aipc::native::RepackNv12(frame));
        const auto detections = npu_.infer(image, {0, 0, 0, 0}, "rgb");
        std::vector<DetectionResult> output;
        output.reserve(detections.size());
        for (const auto& detection : detections) {
            // VisionG exposes Detection.box as (x, y, width, height), while
            // AIPR deliberately uses explicit corner coordinates.
            const auto [x, y, width, height] = detection.box;
            output.push_back(DetectionFromXywh(
                x, y, width, height, detection.score, detection.class_id,
                detection.label));
        }
        return output;
    }

    const char* Name() const override { return "visiong"; }

private:
    NPU npu_;
};
#endif

}  // namespace

DetectionResult DetectionFromXywh(int x, int y, int width, int height,
                                  float score, int class_id,
                                  std::string label) {
    return {x, y, SaturatingAdd(x, width), SaturatingAdd(y, height),
            score, class_id, std::move(label)};
}

std::unique_ptr<Backend> CreateMockBackend() {
    return std::make_unique<MockBackend>();
}

std::unique_ptr<Backend> CreateBackend(const Options& options,
                                       const Manifest& manifest) {
    if (options.mock) return CreateMockBackend();
#if AIPC_ENABLE_VISIONG
    const fs::path model = options.models_dir / manifest.model;
    const fs::path labels = options.models_dir / manifest.labels;
    if (!fs::is_regular_file(model)) {
        throw std::runtime_error("model file does not exist");
    }
    if (!manifest.labels.empty() && !fs::is_regular_file(labels)) {
        throw std::runtime_error("labels file does not exist");
    }
    return std::make_unique<VisionGBackend>(model, labels, manifest.threshold,
                                            manifest.nms_threshold);
#else
    (void)manifest;
    throw std::runtime_error(
        "ai_worker was built without VisionG; use --mock for tests");
#endif
}

}  // namespace ai_worker
