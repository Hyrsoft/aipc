#include "application.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#include "aipc/native/aipf.h"
#include "aipc/native/io.h"
#include "backend.h"
#include "lua_runtime.h"
#include "manifest.h"
#include "types.h"

namespace ai_worker {
namespace {

Options ParseOptions(int argc, char* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value = [&](const char* name) {
            if (++index >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return std::string(argv[index]);
        };
        if (argument == "--project-dir") {
            options.project_dir = value("--project-dir");
        } else if (argument == "--models-dir") {
            options.models_dir = value("--models-dir");
        } else if (argument == "--input-fd") {
            options.input_fd = std::stoi(value("--input-fd"));
        } else if (argument == "--output-fd") {
            options.output_fd = std::stoi(value("--output-fd"));
        } else if (argument == "--validate-only") {
            options.validate_only = true;
        } else if (argument == "--mock") {
            options.mock = true;
        } else if (argument == "--probe-load") {
            options.probe_load = true;
        } else {
            throw std::runtime_error("unknown option: " + argument);
        }
    }
    if (!options.probe_load &&
        (options.project_dir.empty() || options.models_dir.empty())) {
        throw std::runtime_error("--project-dir and --models-dir are required");
    }
    return options;
}

}  // namespace

int Run(int argc, char* argv[]) {
    const Options options = ParseOptions(argc, argv);
    if (options.probe_load) {
        std::cout << R"({"loaded":true,"worker":"ai_worker"})" << '\n';
        return 0;
    }
    const Manifest manifest = LoadManifest(options.project_dir);
    if (!fs::is_regular_file(options.project_dir / manifest.entry)) {
        throw std::runtime_error("Lua entry file does not exist");
    }
    if (!options.mock) {
        for (const auto& file : ReferencedFiles(manifest)) {
            if (!fs::is_regular_file(options.models_dir / file)) {
                throw std::runtime_error("AI resource does not exist: " + file);
            }
        }
    }
    if (options.validate_only) {
        LuaRuntime runtime(manifest, options.project_dir, CreateMockBackend());
        std::cout << json{{"valid", true}, {"project", manifest.id}}.dump()
                  << '\n';
        return 0;
    }
    LuaRuntime runtime(manifest, options.project_dir,
                       CreateBackend(options, manifest));
    if (!aipc::native::WriteJsonMessage(
            options.output_fd,
            {{"version", 1},
             {"type", "worker_ready"},
             {"project", manifest.id},
             {"algorithm", manifest.algorithm},
             {"backend", runtime.BackendName()},
             {"visiong_version",
              AIPC_ENABLE_VISIONG ? json("1.2.1") : json(nullptr)}})) {
        throw std::runtime_error("cannot publish worker_ready");
    }
    std::uint64_t errors = 0;
    while (true) {
        std::string error;
        auto frame = aipc::native::ReadAipfFrame(options.input_fd, &error);
        if (!frame) {
            if (!error.empty()) throw std::runtime_error(error);
            break;
        }
        const auto started = std::chrono::steady_clock::now();
        try {
            json detections = runtime.Process(*frame);
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started)
                    .count();
            if (!aipc::native::WriteJsonMessage(
                    options.output_fd,
                    {{"version", 1},
                     {"type", "inference_result"},
                     {"sequence", frame->sequence},
                     {"pts", frame->pts},
                     {"width", frame->width},
                     {"height", frame->height},
                     {"inference_us", elapsed},
                     {"detections", std::move(detections)}})) {
                break;
            }
            errors = 0;
        } catch (const std::exception& exception) {
            ++errors;
            aipc::native::WriteJsonMessage(
                options.output_fd,
                {{"version", 1},
                 {"type", "worker_error"},
                 {"stage", "process"},
                 {"recoverable", errors < 3},
                 {"error", exception.what()}});
            if (errors >= 3) throw;
        }
    }
    return 0;
}

}  // namespace ai_worker
