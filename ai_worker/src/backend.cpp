#include "backend.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#if AIPC_ENABLE_VISIONG
#include <visiong/core/ImageBuffer.h>
#include <visiong/core/npu_clock.h>
#include <visiong/modules/IVE.h>
#include <visiong/npu/NPU.h>
#include <visiong/npu/NanoTrack.h>
#include <visiong/npu/PPOCR.h>
#endif

namespace ai_worker {
namespace {

int SaturatingAdd(int value, int delta) {
    const auto result = static_cast<long long>(value) +
                        static_cast<long long>(std::max(0, delta));
    return static_cast<int>(std::min(
        result, static_cast<long long>(std::numeric_limits<int>::max())));
}

#if AIPC_ENABLE_VISIONG
json MergeOptions(const json& defaults, const json& overrides) {
    json output = defaults.is_object() ? defaults : json::object();
    if (overrides.is_object()) {
        for (auto item = overrides.begin(); item != overrides.end(); ++item) {
            output[item.key()] = item.value();
        }
    }
    return output;
}
#endif

class MockBackend final : public Backend {
public:
    json Infer(const Frame& frame, const json&) override {
        const int width = static_cast<int>(frame.width);
        const int height = static_cast<int>(frame.height);
        const int offset =
            static_cast<int>(frame.sequence % std::max(1, width / 4));
        return json::array({ObjectFromXywh(
            offset, height / 4, std::max(1, width / 3), std::max(1, height / 2),
            0.9F, 0, "mock")});
    }
    const char* Name() const override { return "mock"; }
};

#if AIPC_ENABLE_VISIONG

ImageBuffer FrameImage(const Frame& frame) {
    return ImageBuffer(static_cast<int>(frame.width),
                       static_cast<int>(frame.height), RK_FMT_YUV420SP,
                       aipc::native::RepackNv12(frame));
}

fs::path Resource(const fs::path& root, const Manifest& manifest,
                  const std::string& role) {
    if (role == "model") {
        return manifest.model.empty() ? fs::path{} : root / manifest.model;
    }
    if (role == "labels") {
        return manifest.labels.empty() ? fs::path{} : root / manifest.labels;
    }
    const auto found = manifest.files.find(role);
    if (found == manifest.files.end()) return {};
    return root / found->second;
}

json Points2(const std::vector<std::tuple<float, float>>& points) {
    json output = json::array();
    for (const auto& [x, y] : points) output.push_back({x, y});
    return output;
}

json Points3(const std::vector<std::tuple<float, float, float>>& points) {
    json output = json::array();
    for (const auto& [x, y, score] : points) output.push_back({x, y, score});
    return output;
}

class NpuBackend final : public Backend {
public:
    NpuBackend(const fs::path& root, const Manifest& manifest, ModelType type)
        : algorithm_(manifest.algorithm), defaults_(manifest.options),
          npu_(type, Resource(root, manifest, "model").string(),
               Resource(root, manifest, "labels").string(),
               manifest.threshold, manifest.nms_threshold) {
        if (!npu_.is_initialized()) {
            throw std::runtime_error("VisionG NPU initialization failed");
        }
    }

    json Infer(const Frame& frame, const json& overrides) override {
        const json options = MergeOptions(defaults_, overrides);
        ImageBuffer image = FrameImage(frame);
        if (algorithm_ == "lprnet") {
            const std::string plate = npu_.recognize_plate(image);
            if (plate.empty()) return json::array();
            json item = ObjectFromXywh(0, 0, static_cast<int>(frame.width),
                                       static_cast<int>(frame.height), 1.0F, 0,
                                       plate, "text");
            item["text"] = plate;
            return json::array({std::move(item)});
        }
        if (algorithm_ == "mlsd") {
            const float score = options.value("score_threshold", 0.25F);
            const float distance = options.value("distance_threshold", 30.0F);
            const auto lines = npu_.infer_lines(
                image, score, distance, options.value("model_format", "rgb"));
            json output = json::array();
            const std::size_t limit = static_cast<std::size_t>(
                std::max(1, options.value("max_lines", 40)));
            for (const auto& line : lines) {
                if (output.size() >= limit) break;
                const int x1 = static_cast<int>(std::lround(line.x1));
                const int y1 = static_cast<int>(std::lround(line.y1));
                const int x2 = static_cast<int>(std::lround(line.x2));
                const int y2 = static_cast<int>(std::lround(line.y2));
                json item{{"x1", x1},
                          {"y1", y1},
                          {"x2", x2},
                          {"y2", y2},
                          {"confidence", line.score},
                          {"class_id", 0},
                          {"label", "line"},
                          {"kind", "line"},
                          {"length", line.length}};
                output.push_back(std::move(item));
            }
            return output;
        }

        const auto detections = npu_.infer(
            image, {0, 0, 0, 0}, options.value("model_format", "rgb"));
        json output = json::array();
        for (const auto& detection : detections) {
            const auto [x, y, width, height] = detection.box;
            json item = ObjectFromXywh(x, y, width, height, detection.score,
                                       detection.class_id, detection.label);
            if (!detection.landmarks.empty()) {
                item["landmarks"] = Points2(detection.landmarks);
            }
            if (!detection.keypoints.empty()) {
                item["keypoints"] = Points3(detection.keypoints);
            }
            if (!detection.mask_points.empty()) {
                item["mask_points"] = Points2(detection.mask_points);
            }
            output.push_back(std::move(item));
        }
        return output;
    }

    const char* Name() const override { return "visiong"; }

private:
    std::string algorithm_;
    json defaults_;
    NPU npu_;
};

class PpocrBackend final : public Backend {
public:
    PpocrBackend(const fs::path& root, const Manifest& manifest)
        : defaults_(manifest.options),
          ocr_(Resource(root, manifest, "model").string(),
               Resource(root, manifest, "recognizer").string(),
               Resource(root, manifest, "dictionary").string(),
               defaults_.value("det_threshold", 0.3F),
               defaults_.value("box_threshold", 0.5F),
               defaults_.value("use_dilate", true),
               Resource(root, manifest, "recognizer_fast").string(),
               defaults_.value("rec_fast_max_ratio", 9.0F),
               defaults_.value("rec_fast_enable_fallback", true),
               defaults_.value("rec_fast_fallback_score_thresh", 0.2F),
               defaults_.value("model_input_format", "rgb"),
               defaults_.value("det_unclip_ratio", 1.6F)) {
        if (!ocr_.is_initialized()) {
            throw std::runtime_error("VisionG PPOCR initialization failed");
        }
    }

    json Infer(const Frame& frame, const json&) override {
        ImageBuffer image = FrameImage(frame);
        json output = json::array();
        for (const auto& result : ocr_.infer(image)) {
            const auto [x, y, width, height] = result.rect;
            json item = ObjectFromXywh(x, y, width, height, result.text_score,
                                       0, result.text, "text");
            item["text"] = result.text;
            item["text_score"] = result.text_score;
            item["det_score"] = result.det_score;
            json quad = json::array();
            for (const auto& [px, py] : result.quad) quad.push_back({px, py});
            item["quad"] = std::move(quad);
            output.push_back(std::move(item));
        }
        return output;
    }

    const char* Name() const override { return "visiong"; }

private:
    json defaults_;
    PPOCR ocr_;
};

std::tuple<int, int, int, int> ReadBox(const json& value, int width,
                                       int height) {
    if (value.is_array() && value.size() == 4) {
        return {value[0].get<int>(), value[1].get<int>(), value[2].get<int>(),
                value[3].get<int>()};
    }
    if (value.is_object()) {
        return {value.value("x", 0), value.value("y", 0),
                value.value("width", width / 4),
                value.value("height", height / 4)};
    }
    const int box_width = std::max(16, width / 4);
    const int box_height = std::max(16, height / 4);
    return {(width - box_width) / 2, (height - box_height) / 2, box_width,
            box_height};
}

class NanoTrackBackend final : public Backend {
public:
    NanoTrackBackend(const fs::path& root, const Manifest& manifest)
        : defaults_(manifest.options),
          tracker_(Resource(root, manifest, "model").string(),
                   Resource(root, manifest, "search").string(),
                   Resource(root, manifest, "head").string()) {}

    json Infer(const Frame& frame, const json& overrides) override {
        const json options = MergeOptions(defaults_, overrides);
        ImageBuffer image = FrameImage(frame);
        const std::string action = options.value("action", "track");
        if (action == "reset") tracker_.reset();
        if (!tracker_.is_initialized() || action == "init" || action == "reset") {
            const auto box = ReadBox(options.value("box", json()),
                                     static_cast<int>(frame.width),
                                     static_cast<int>(frame.height));
            tracker_.init(image, box);
            const auto [x, y, width, height] = box;
            json item = ObjectFromXywh(x, y, width, height, 1.0F, 0,
                                       "target", "track");
            item["initialized"] = true;
            return json::array({std::move(item)});
        }
        const auto result = tracker_.track(image);
        const auto [x, y, width, height] = result.box;
        return json::array({ObjectFromXywh(x, y, width, height, result.score,
                                          0, "target", "track")});
    }

    const char* Name() const override { return "visiong"; }

private:
    json defaults_;
    NanoTrack tracker_;
};

class FindBlobsBackend final : public Backend {
public:
    explicit FindBlobsBackend(const Manifest& manifest)
        : defaults_(manifest.options) {}

    json Infer(const Frame& frame, const json& overrides) override {
        const json options = MergeOptions(defaults_, overrides);
        std::vector<std::tuple<int, int, int, int, int, int>> thresholds;
        const json configured = options.value(
            "thresholds", json::array({json::array({0, 131, 161, 255, 0, 255})}));
        for (const auto& value : configured) {
            if (!value.is_array() || value.size() != 6) {
                throw std::runtime_error("find_blobs thresholds require six values");
            }
            thresholds.emplace_back(value[0].get<int>(), value[1].get<int>(),
                                    value[2].get<int>(), value[3].get<int>(),
                                    value[4].get<int>(), value[5].get<int>());
        }
        ImageBuffer image = FrameImage(frame);
        const auto blobs = image.find_blobs(
            thresholds, options.value("invert", false), {0, 0, 0, 0},
            options.value("x_stride", 2), options.value("y_stride", 2),
            options.value("area_threshold", 150),
            options.value("pixels_threshold", 150),
            options.value("merge", true), options.value("margin", 10));
        json output = json::array();
        for (const auto& blob : blobs) {
            json item = ObjectFromXywh(blob.x, blob.y, blob.w, blob.h, 1.0F,
                                       static_cast<int>(blob.code), "blob",
                                       "blob");
            item["pixels"] = blob.pixels;
            item["center"] = {blob.cx, blob.cy};
            output.push_back(std::move(item));
        }
        return output;
    }

    const char* Name() const override { return "visiong"; }

private:
    json defaults_;
};

class IveFilterBackend final : public Backend {
public:
    explicit IveFilterBackend(const Manifest& manifest)
        : defaults_(manifest.options) {}

    json Infer(const Frame& frame, const json& overrides) override {
        const json options = MergeOptions(defaults_, overrides);
        const json configured = options.value(
            "kernel", json::array({1, 4, 6, 4, 1, 4, 16, 24, 16, 4, 6, 24,
                                    36, 24, 6, 4, 16, 24, 16, 4, 1, 4, 6, 4, 1}));
        if (!configured.is_array() || configured.size() != 25) {
            throw std::runtime_error("IVE filter kernel must contain 25 values");
        }
        std::vector<std::int8_t> kernel;
        kernel.reserve(25);
        for (const auto& value : configured) {
            const int coefficient = value.get<int>();
            if (coefficient < -128 || coefficient > 127) {
                throw std::runtime_error("IVE filter coefficient out of range");
            }
            kernel.push_back(static_cast<std::int8_t>(coefficient));
        }
        ImageBuffer image = FrameImage(frame).to_grayscale();
        ImageBuffer filtered = IVE::get_instance().filter(image, kernel);
        if (!filtered.is_valid()) throw std::runtime_error("IVE filter failed");
        return json::array();
    }

    const char* Name() const override { return "visiong"; }

private:
    json defaults_;
};

class IveNccBackend final : public Backend {
public:
    IveNccBackend(const fs::path& root, const Manifest& manifest)
        : template_(ImageBuffer::load(Resource(root, manifest, "model").string())) {
        if (!template_.is_valid()) throw std::runtime_error("NCC template is invalid");
        template_ = template_.to_grayscale();
    }

    json Infer(const Frame& frame, const json&) override {
        ImageBuffer image = FrameImage(frame).to_grayscale();
        if (image.width != template_.width || image.height != template_.height) {
            image = image.resize(template_.width, template_.height);
        }
        const double score = IVE::get_instance().ncc(image, template_);
        json item = ObjectFromXywh(0, 0, static_cast<int>(frame.width),
                                   static_cast<int>(frame.height),
                                   static_cast<float>(std::clamp(score, 0.0, 1.0)),
                                   0, "template", "similarity");
        item["similarity"] = score;
        return json::array({std::move(item)});
    }

    const char* Name() const override { return "visiong"; }

private:
    ImageBuffer template_;
};

class NpuClockBackend final : public Backend {
public:
    explicit NpuClockBackend(const Manifest& manifest) {
        const bool apply = manifest.options.value("apply", false);
        if (apply) {
            const auto result = clock_.set_rate_mhz(
                manifest.options.value("rate_mhz", 420U),
                manifest.options.value("update_cru_clk500m_src", true),
                manifest.options.value("unbind_rebind_npu", false),
                manifest.options.value("allow_unsafe_rate", false));
            status_ = {{"ok", result.ok},
                       {"message", result.message},
                       {"requested_rate_hz", result.requested_rate_hz},
                       {"current_rate_hz", result.current_rate_hz},
                       {"reboot_required", result.reboot_required}};
        } else {
            const auto result = clock_.status();
            status_ = {{"ok", result.npu_node_present},
                       {"message", result.note},
                       {"current_rate_hz", result.current_rate_hz},
                       {"assigned_rate_hz", result.assigned_rate_hz}};
        }
    }

    json Infer(const Frame&, const json&) override { return json::array(); }
    const char* Name() const override { return "visiong"; }

private:
    visiong::pinmux::NpuClock clock_;
    json status_;
};

class FrameInfoBackend final : public Backend {
public:
    json Infer(const Frame&, const json&) override { return json::array(); }
    const char* Name() const override { return "aipc"; }
};

#endif

}  // namespace

json ObjectFromXywh(int x, int y, int width, int height, float score,
                    int class_id, std::string label, std::string kind) {
    return {{"x1", x},
            {"y1", y},
            {"x2", SaturatingAdd(x, width)},
            {"y2", SaturatingAdd(y, height)},
            {"confidence", std::clamp(score, 0.0F, 1.0F)},
            {"class_id", class_id},
            {"label", std::move(label)},
            {"kind", std::move(kind)}};
}

std::unique_ptr<Backend> CreateMockBackend() {
    return std::make_unique<MockBackend>();
}

std::unique_ptr<Backend> CreateBackend(const Options& options,
                                       const Manifest& manifest) {
    if (options.mock) return CreateMockBackend();
#if AIPC_ENABLE_VISIONG
    if (manifest.algorithm == "yolov5") {
        return std::make_unique<NpuBackend>(options.models_dir, manifest,
                                            ModelType::YOLOV5);
    }
    if (manifest.algorithm == "yolo11") {
        return std::make_unique<NpuBackend>(options.models_dir, manifest,
                                            ModelType::YOLO11);
    }
    if (manifest.algorithm == "lprnet") {
        return std::make_unique<NpuBackend>(options.models_dir, manifest,
                                            ModelType::LPRNET);
    }
    if (manifest.algorithm == "mlsd") {
        return std::make_unique<NpuBackend>(options.models_dir, manifest,
                                            ModelType::MLSD);
    }
    if (manifest.algorithm == "ppocr") {
        return std::make_unique<PpocrBackend>(options.models_dir, manifest);
    }
    if (manifest.algorithm == "nanotrack") {
        return std::make_unique<NanoTrackBackend>(options.models_dir, manifest);
    }
    if (manifest.algorithm == "find_blobs") {
        return std::make_unique<FindBlobsBackend>(manifest);
    }
    if (manifest.algorithm == "ive_filter") {
        return std::make_unique<IveFilterBackend>(manifest);
    }
    if (manifest.algorithm == "ive_ncc") {
        return std::make_unique<IveNccBackend>(options.models_dir, manifest);
    }
    if (manifest.algorithm == "npu_clock") {
        return std::make_unique<NpuClockBackend>(manifest);
    }
    if (manifest.algorithm == "frame_info") {
        return std::make_unique<FrameInfoBackend>();
    }
    throw std::runtime_error("unsupported VisionG backend algorithm");
#else
    (void)manifest;
    throw std::runtime_error(
        "ai_worker was built without VisionG; use --mock for tests");
#endif
}

}  // namespace ai_worker
