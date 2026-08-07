#include "manifest.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

#include "aipc/native/validation.h"

namespace ai_worker {

bool SafeName(const std::string& value) {
    return aipc::native::IsSafeFileName(value);
}

Manifest LoadManifest(const fs::path& project_dir) {
    std::ifstream input(project_dir / "manifest.json");
    if (!input) throw std::runtime_error("cannot open manifest.json");
    json raw;
    input >> raw;
    Manifest manifest;
    manifest.raw = raw;
    manifest.id = raw.value("id", project_dir.filename().string());
    manifest.name = raw.value("name", manifest.id);
    manifest.entry = raw.value("entry", manifest.entry);
    manifest.algorithm = raw.value("algorithm", manifest.algorithm);
    manifest.model = raw.value("model", "");
    manifest.labels = raw.value("labels", "");
    manifest.threshold = raw.value("threshold", manifest.threshold);
    manifest.nms_threshold = raw.value("nms_threshold", manifest.nms_threshold);
    manifest.max_detections = raw.value("max_detections", manifest.max_detections);
    manifest.class_filter = raw.value("class_filter", manifest.class_filter);
    if (!SafeName(manifest.id) || !SafeName(manifest.entry) ||
        !SafeName(manifest.model) ||
        (!manifest.labels.empty() && !SafeName(manifest.labels))) {
        throw std::runtime_error("manifest contains an unsafe project or file name");
    }
    if (manifest.algorithm != "yolov5") {
        throw std::runtime_error("only yolov5 is supported by ai_worker v1");
    }
    if (!(manifest.threshold >= 0.0F && manifest.threshold <= 1.0F) ||
        !(manifest.nms_threshold >= 0.0F && manifest.nms_threshold <= 1.0F) ||
        manifest.max_detections < 1 || manifest.max_detections > 256) {
        throw std::runtime_error("invalid inference thresholds or max_detections");
    }
    if (manifest.class_filter.size() > 256 ||
        std::any_of(manifest.class_filter.begin(), manifest.class_filter.end(),
                    [](int value) { return value < 0 || value > 10000; })) {
        throw std::runtime_error("invalid class_filter");
    }
    return manifest;
}

}  // namespace ai_worker
