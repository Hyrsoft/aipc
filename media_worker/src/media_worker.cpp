#include "media_worker.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <chrono>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include "rk_mpi_sys.h"
#include "sample_comm.h"

namespace media_worker {

MediaWorker::MediaWorker(WorkerConfig config)
    : config_(std::move(config)), events_(config_.runtime.generation) {}

MediaWorker::~MediaWorker() {
    Shutdown();
}

bool MediaWorker::PreflightOutputs(std::string* error) {
    auto check = [error](const std::string& media, const std::string& path) {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        if (file == nullptr) {
            *error = "cannot open " + media + " output " + path + ": " +
                     std::strerror(errno);
            return false;
        }
        std::fclose(file);
        return true;
    };
    if (!config_.video.output_path.empty() && !check("video", config_.video.output_path)) return false;
    if (config_.audio.enabled && !config_.audio.output_path.empty() &&
        !check("audio", config_.audio.output_path)) return false;
    return true;
}

bool MediaWorker::InitSharedRuntime(std::string* error) {
    events_.Emit("BootProgress", {{"stage", "isp_initializing"}});
    RK_S32 result = SAMPLE_COMM_ISP_Init(config_.isp.camera_id, RK_AIQ_WORKING_MODE_NORMAL,
                                         RK_FALSE, config_.isp.iq_dir.c_str());
    if (result != RK_SUCCESS) {
        *error = "SAMPLE_COMM_ISP_Init failed: " + std::to_string(result);
        return false;
    }
    isp_initialized_ = true;
    result = SAMPLE_COMM_ISP_Run(config_.isp.camera_id);
    if (result != RK_SUCCESS) {
        *error = "SAMPLE_COMM_ISP_Run failed: " + std::to_string(result);
        return false;
    }
    isp_running_ = true;
    events_.Emit("BootProgress", {{"stage", "isp_ready"}});

    result = RK_MPI_SYS_Init();
    if (result != RK_SUCCESS) {
        *error = "RK_MPI_SYS_Init failed: " + std::to_string(result);
        return false;
    }
    mpi_initialized_ = true;
    events_.Emit("BootProgress", {{"stage", "mpi_ready"}});
    return true;
}

void MediaWorker::DeinitSharedRuntime() {
    if (mpi_initialized_) {
        RK_MPI_SYS_Exit();
        mpi_initialized_ = false;
    }
    if (isp_running_ || isp_initialized_) {
        SAMPLE_COMM_ISP_Stop(config_.isp.camera_id);
        isp_running_ = false;
        isp_initialized_ = false;
    }
}

void MediaWorker::RequestFatalStop(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        if (runtime_error_.empty()) runtime_error_ = message;
    }
    stop_requested_.store(true);
}

std::string MediaWorker::RuntimeError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return runtime_error_;
}

void MediaWorker::EmitMetrics(double elapsed_seconds) {
    nlohmann::json fields;
    fields["elapsed_seconds"] = elapsed_seconds;
    if (video_) {
        fields["video"] = video_->Stats();
        const auto rates = video_metrics_sampler_.Sample(
            fields["video"]["packets"].get<std::uint64_t>(),
            fields["video"]["bytes"].get<std::uint64_t>(), elapsed_seconds);
        fields["video"]["fps"] = rates.fps;
        fields["video"]["bitrate_kbps"] = rates.bitrate_kbps;
        fields["video"]["average_fps"] =
            elapsed_seconds > 0 ? fields["video"]["packets"].get<double>() / elapsed_seconds : 0;
        fields["video"]["average_bitrate_kbps"] =
            elapsed_seconds > 0
                ? fields["video"]["bytes"].get<double>() * 8.0 / elapsed_seconds / 1000.0
                : 0;
    }
    if (audio_) {
        fields["audio"] = audio_->Stats();
        const auto rates = audio_metrics_sampler_.Sample(
            fields["audio"]["packets"].get<std::uint64_t>(),
            fields["audio"]["bytes"].get<std::uint64_t>(), elapsed_seconds);
        fields["audio"]["bitrate_kbps"] = rates.bitrate_kbps;
        fields["audio"]["average_bitrate_kbps"] =
            elapsed_seconds > 0
                ? fields["audio"]["bytes"].get<double>() * 8.0 / elapsed_seconds / 1000.0
                : 0;
    }
    events_.Emit("Metrics", std::move(fields));
}

int MediaWorker::Run(const std::function<bool()>& external_stop_requested) {
    std::string error;
    events_.Emit("BootProgress",
                 {{"stage", "config_loaded"},
                  {"video_output", config_.video.output_path},
                  {"audio_enabled", config_.audio.enabled},
                  {"audio_output", config_.audio.output_path}});

    // Vendor ISP/RKMPI libraries print diagnostics to stdout. Keep stdout reserved for
    // JSONL events by redirecting subsequent ordinary stdout writes to stderr. EventEmitter
    // owns a duplicate of the original stdout descriptor and remains unaffected.
    if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        std::cerr << "warning: failed to redirect vendor stdout to stderr: "
                  << std::strerror(errno) << '\n';
    }

    if (!PreflightOutputs(&error)) {
        events_.Emit("FatalError", {{"stage", "preflight"}, {"message", error}});
        events_.Emit("Stopped", {{"reason", "initialization_error"},
                                  {"exit_code", static_cast<int>(ExitCode::kInitializationError)}});
        return static_cast<int>(ExitCode::kInitializationError);
    }
    if (!InitSharedRuntime(&error)) {
        events_.Emit("FatalError", {{"stage", "shared_runtime"}, {"message", error}});
        Shutdown();
        events_.Emit("Stopped", {{"reason", "initialization_error"},
                                  {"exit_code", static_cast<int>(ExitCode::kInitializationError)}});
        return static_cast<int>(ExitCode::kInitializationError);
    }

    auto fatal_callback = [this](const std::string& message) { RequestFatalStop(message); };
    video_ = std::make_unique<VideoPipeline>(config_, &events_, fatal_callback);
    if (!video_->Init(&error) || !video_->Start(&error)) {
        events_.Emit("FatalError", {{"stage", "video_init"}, {"message", error}});
        Shutdown();
        events_.Emit("Stopped", {{"reason", "initialization_error"},
                                  {"exit_code", static_cast<int>(ExitCode::kInitializationError)}});
        return static_cast<int>(ExitCode::kInitializationError);
    }

    if (config_.audio.enabled) {
        audio_ = std::make_unique<AudioPipeline>(config_, &events_, fatal_callback);
        if (!audio_->Init(&error) || !audio_->Start(&error)) {
            events_.Emit("FatalError", {{"stage", "audio_init"}, {"message", error}});
            Shutdown();
            events_.Emit(
                "Stopped",
                {{"reason", "initialization_error"},
                 {"exit_code", static_cast<int>(ExitCode::kInitializationError)}});
            return static_cast<int>(ExitCode::kInitializationError);
        }
    }

    if (config_.ai_input.control_fd >= 0) {
        control_ = std::make_unique<MediaControl>(
            config_.ai_input.control_fd,
            [this](const nlohmann::json& request) { return HandleControl(request); });
        if (!control_->Start()) {
            events_.Emit("Warning", {{"media", "control"},
                                      {"reason", "control_start_failed"}});
        }
    }

    events_.Emit("BootProgress", {{"stage", "running"}});
    const auto started_at = std::chrono::steady_clock::now();
    auto next_metrics = started_at +
                        std::chrono::milliseconds(config_.runtime.metrics_interval_ms);
    std::string stop_reason = "signal";
    while (!stop_requested_.load()) {
        if (external_stop_requested()) {
            stop_reason = "signal";
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (config_.runtime.duration_sec > 0 &&
            now - started_at >= std::chrono::seconds(config_.runtime.duration_sec)) {
            stop_reason = "duration_elapsed";
            break;
        }
        if (now >= next_metrics) {
            const double elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - started_at).count() /
                1000.0;
            EmitMetrics(elapsed);
            next_metrics = now + std::chrono::milliseconds(config_.runtime.metrics_interval_ms);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    const std::string runtime_error = RuntimeError();
    if (!runtime_error.empty()) stop_reason = "runtime_error";
    const auto stopped_at = std::chrono::steady_clock::now();
    const double elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(stopped_at - started_at).count() /
        1000.0;
    EmitMetrics(elapsed);
    Shutdown();

    const ExitCode exit_code =
        runtime_error.empty() ? ExitCode::kSuccess : ExitCode::kRuntimeError;
    nlohmann::json stopped = {{"reason", stop_reason},
                              {"exit_code", static_cast<int>(exit_code)}};
    if (!runtime_error.empty()) stopped["message"] = runtime_error;
    events_.Emit("Stopped", std::move(stopped));
    return static_cast<int>(exit_code);
}

void MediaWorker::Shutdown() {
    if (control_) {
        control_->Stop();
        control_.reset();
    }
    if (audio_) audio_->Stop();
    if (video_) video_->Stop();
    if (audio_) {
        audio_->Deinit();
        audio_.reset();
    }
    if (video_) {
        video_->Deinit();
        video_.reset();
    }
    DeinitSharedRuntime();
}

nlohmann::json MediaWorker::HandleControl(const nlohmann::json& request) {
    const std::string request_id = request.value("request_id", "");
    auto response = nlohmann::json{{"version", 1},
                                   {"request_id", request_id},
                                   {"type", "ack"}};
    if (request.value("version", 0) != 1 || request_id.empty()) {
        return {{"version", 1},
                {"request_id", request_id},
                {"type", "error"},
                {"error", "invalid control envelope"}};
    }
    const std::string command = request.value("command", "");
    std::string error;
    if (command == "pause_ai_frames") {
        if (!video_->PauseAiFrames(&error)) response["type"] = "error";
    } else if (command == "resume_ai_frames") {
        if (!video_->ResumeAiFrames(&error)) response["type"] = "error";
    } else if (command == "configure_ai_channel") {
        const auto& value = request.at("ai_input");
        AiInputConfig next = config_.ai_input;
        next.enabled = value.value("enabled", next.enabled);
        next.channel_id = value.value("channel_id", next.channel_id);
        next.width = value.value("width", next.width);
        next.height = value.value("height", next.height);
        next.fps = value.value("fps", next.fps);
        next.pixel_format = value.value("pixel_format", next.pixel_format);
        next.fit_mode = value.value("fit_mode", next.fit_mode);
        next.buffer_count = value.value("buffer_count", next.buffer_count);
        next.depth = value.value("depth", next.depth);
        WorkerConfig candidate = config_;
        candidate.ai_input = next;
        const auto errors = ValidateConfig(candidate);
        if (!errors.empty()) {
            response["type"] = "error";
            response["error"] = errors;
            return response;
        }
        if (!video_->ReconfigureAiInput(next, &error)) {
            response["type"] = "error";
        } else {
            config_.ai_input = next;
            response["event"] = "ai_input_ready";
            response["ai_input"] = value;
        }
    } else if (command == "probe_region_capability") {
        const auto capability = video_->ProbeRegionCapability(&error);
        if (!error.empty()) response["type"] = "error";
        response["event"] = "region_capability";
        for (auto item = capability.begin(); item != capability.end(); ++item) {
            response[item.key()] = item.value();
        }
    } else if (command == "set_osd_mode") {
        const std::string mode = request.value("mode", "off");
        if (!video_->SetOsdMode(mode, &error)) {
            response["type"] = "error";
        } else {
            response["mode"] = mode;
        }
    } else if (command == "update_regions") {
        const int main_width = request.value("main_width", 0);
        const int main_height = request.value("main_height", 0);
        if (main_width != config_.video.width || main_height != config_.video.height) {
            response["type"] = "error";
            error = "region coordinate space does not match the main video";
        } else {
            std::vector<OsdRegion> regions;
            for (const auto& value : request.value("regions", nlohmann::json::array())) {
                if (regions.size() >= 32) break;
                OsdRegion region{value.value("x", 0), value.value("y", 0),
                                 value.value("width", 0), value.value("height", 0)};
                if (region.width < 2 || region.height < 2) continue;
                regions.push_back(region);
            }
            if (!video_->UpdateRegions(regions, request.value("ttl_ms", 300), &error)) {
                response["type"] = "error";
            } else {
                response["regions"] = regions.size();
            }
        }
    } else {
        response["type"] = "error";
        error = "unknown media control command";
    }
    if (!error.empty()) response["error"] = error;
    return response;
}

}  // namespace media_worker
