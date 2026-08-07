#include "media_control_service.h"

#include <string>
#include <vector>

#include "rgn_manager.h"
#include "video_pipeline.h"

namespace media_worker {

MediaControlService::MediaControlService(WorkerConfig* config, VideoPipeline* video)
    : config_(config), video_(video) {}

nlohmann::json MediaControlService::Handle(const nlohmann::json& request) {
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
        AiInputConfig next = config_->ai_input;
        next.enabled = value.value("enabled", next.enabled);
        next.channel_id = value.value("channel_id", next.channel_id);
        next.width = value.value("width", next.width);
        next.height = value.value("height", next.height);
        next.fps = value.value("fps", next.fps);
        next.pixel_format = value.value("pixel_format", next.pixel_format);
        next.fit_mode = value.value("fit_mode", next.fit_mode);
        next.buffer_count = value.value("buffer_count", next.buffer_count);
        next.depth = value.value("depth", next.depth);
        WorkerConfig candidate = *config_;
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
            config_->ai_input = next;
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
        if (main_width != config_->video.width || main_height != config_->video.height) {
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
