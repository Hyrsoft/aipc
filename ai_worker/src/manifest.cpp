#include "manifest.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>

#include "aipc/native/validation.h"

namespace ai_worker {

bool SafeName(const std::string& value) {
    return aipc::native::IsSafeFileName(value);
}

bool SupportedAlgorithm(const std::string& value) {
    static const std::set<std::string> algorithms{
        "yolov5",      "yolo11",    "lprnet",   "mlsd",
        "ppocr",       "nanotrack", "find_blobs", "ive_filter",
        "ive_ncc",     "npu_clock", "frame_info",
    };
    return algorithms.count(value) != 0;
}

std::vector<std::string> ReferencedFiles(const Manifest& manifest) {
    std::vector<std::string> files;
    auto append = [&files](const std::string& value) {
        if (!value.empty() &&
            std::find(files.begin(), files.end(), value) == files.end()) {
            files.push_back(value);
        }
    };
    append(manifest.model);
    append(manifest.labels);
    for (const auto& [role, value] : manifest.files) {
        (void)role;
        append(value);
    }
    return files;
}

namespace {

void RequireFileRole(const Manifest& manifest, const char* role) {
    const auto found = manifest.files.find(role);
    if (found == manifest.files.end() || found->second.empty()) {
        throw std::runtime_error("manifest requires files." + std::string(role));
    }
}

}  // namespace

Manifest LoadManifest(const fs::path& project_dir) {
    std::ifstream input(project_dir / "manifest.json");
    if (!input) throw std::runtime_error("cannot open manifest.json");
    json raw;
    input >> raw;
    Manifest manifest;
    manifest.raw = raw;
    manifest.schema_version = raw.value("schema_version", 1);
    manifest.id = raw.value("id", project_dir.filename().string());
    manifest.name = raw.value("name", manifest.id);
    manifest.entry = raw.value("entry", manifest.entry);
    manifest.algorithm = raw.value("algorithm", manifest.algorithm);
    manifest.model = raw.value("model", "");
    manifest.labels = raw.value("labels", "");
    manifest.files = raw.value("files", manifest.files);
    manifest.options = raw.value("options", manifest.options);
    manifest.threshold = raw.value("threshold", manifest.threshold);
    manifest.nms_threshold = raw.value("nms_threshold", manifest.nms_threshold);
    manifest.max_detections = raw.value("max_detections", manifest.max_detections);
    manifest.class_filter = raw.value("class_filter", manifest.class_filter);
    if (!SafeName(manifest.id) || !SafeName(manifest.entry) ||
        (!manifest.model.empty() && !SafeName(manifest.model)) ||
        (!manifest.labels.empty() && !SafeName(manifest.labels))) {
        throw std::runtime_error("manifest contains an unsafe project or file name");
    }
    if (manifest.schema_version < 1 || manifest.schema_version > 2) {
        throw std::runtime_error("unsupported manifest schema_version");
    }
    if (!SupportedAlgorithm(manifest.algorithm)) {
        throw std::runtime_error("unsupported AI algorithm: " + manifest.algorithm);
    }
    if (!manifest.options.is_object() || manifest.options.size() > 64 ||
        manifest.files.size() > 32) {
        throw std::runtime_error("invalid algorithm files or options");
    }
    for (const auto& [role, value] : manifest.files) {
        if (!SafeName(role) || !SafeName(value)) {
            throw std::runtime_error("manifest contains an unsafe resource role or file name");
        }
    }
    if (manifest.algorithm == "yolov5" || manifest.algorithm == "yolo11" ||
        manifest.algorithm == "lprnet" || manifest.algorithm == "mlsd" ||
        manifest.algorithm == "ive_ncc") {
        if (manifest.model.empty()) {
            throw std::runtime_error("algorithm requires model");
        }
    } else if (manifest.algorithm == "ppocr") {
        if (manifest.model.empty()) throw std::runtime_error("ppocr requires detector model");
        RequireFileRole(manifest, "recognizer");
        RequireFileRole(manifest, "dictionary");
    } else if (manifest.algorithm == "nanotrack") {
        if (manifest.model.empty()) throw std::runtime_error("nanotrack requires template model");
        RequireFileRole(manifest, "search");
        RequireFileRole(manifest, "head");
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
