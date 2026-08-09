#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "aipc/native/aipf.h"

namespace ai_worker {

namespace fs = std::filesystem;
using json = nlohmann::json;
using Frame = aipc::native::AiFrame;

struct Options {
    fs::path project_dir;
    fs::path models_dir;
    int input_fd = 3;
    int output_fd = 4;
    bool validate_only = false;
    bool mock = false;
    bool probe_load = false;
};

struct Manifest {
    int schema_version = 2;
    std::string id;
    std::string name;
    std::string entry = "main.lua";
    std::string algorithm = "yolov5";
    std::string model;
    std::string labels;
    std::map<std::string, std::string> files;
    json options = json::object();
    float threshold = 0.25F;
    float nms_threshold = 0.45F;
    int max_detections = 32;
    std::vector<int> class_filter;
    json raw;
};

}  // namespace ai_worker
