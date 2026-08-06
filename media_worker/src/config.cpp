#include "config.h"

#include <cerrno>
#include <climits>
#include <cstdlib>

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace media_worker {
namespace {

using json = nlohmann::json;

bool ParseInt(const std::string& text, int* value) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    long parsed = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' || parsed < INT_MIN ||
        parsed > INT_MAX) {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

bool RequireValue(int argc, char* argv[], int* index, const char* option,
                  std::string* value, std::string* error) {
    if (*index + 1 >= argc) {
        *error = std::string("missing value for ") + option;
        return false;
    }
    *value = argv[++(*index)];
    return true;
}

bool RequireInt(int argc, char* argv[], int* index, const char* option,
                std::optional<int>* value, std::string* error) {
    std::string text;
    if (!RequireValue(argc, argv, index, option, &text, error)) {
        return false;
    }
    int parsed = 0;
    if (!ParseInt(text, &parsed)) {
        *error = std::string("invalid integer for ") + option + ": " + text;
        return false;
    }
    *value = parsed;
    return true;
}

template <typename T>
void ReadIfPresent(const json& object, const char* key, T* value) {
    auto it = object.find(key);
    if (it != object.end()) {
        *value = it->get<T>();
    }
}

}  // namespace

bool ParseCli(int argc, char* argv[], CliOptions* options, std::string* error) {
    if (options == nullptr || error == nullptr) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--help" || arg == "-h") {
            options->help = true;
        } else if (arg == "--config") {
            if (!RequireValue(argc, argv, &i, "--config", &options->config_path, error)) {
                return false;
            }
        } else if (arg == "--generation") {
            if (!RequireValue(argc, argv, &i, "--generation", &value, error)) {
                return false;
            }
            options->generation = value;
        } else if (arg == "--duration-sec") {
            if (!RequireInt(argc, argv, &i, "--duration-sec", &options->duration_sec, error)) {
                return false;
            }
        } else if (arg == "--width") {
            if (!RequireInt(argc, argv, &i, "--width", &options->width, error)) {
                return false;
            }
        } else if (arg == "--height") {
            if (!RequireInt(argc, argv, &i, "--height", &options->height, error)) {
                return false;
            }
        } else if (arg == "--fps") {
            if (!RequireInt(argc, argv, &i, "--fps", &options->fps, error)) {
                return false;
            }
        } else if (arg == "--bitrate-kbps") {
            if (!RequireInt(argc, argv, &i, "--bitrate-kbps", &options->bitrate_kbps,
                            error)) {
                return false;
            }
        } else if (arg == "--gop") {
            if (!RequireInt(argc, argv, &i, "--gop", &options->gop, error)) {
                return false;
            }
        } else if (arg == "--video-output") {
            if (!RequireValue(argc, argv, &i, "--video-output", &value, error)) {
                return false;
            }
            options->video_output = value;
        } else if (arg == "--audio-output") {
            if (!RequireValue(argc, argv, &i, "--audio-output", &value, error)) {
                return false;
            }
            options->audio_output = value;
        } else if (arg == "--iq-dir") {
            if (!RequireValue(argc, argv, &i, "--iq-dir", &value, error)) {
                return false;
            }
            options->iq_dir = value;
        } else if (arg == "--video-ipc-fd") {
            if (!RequireInt(argc, argv, &i, "--video-ipc-fd", &options->video_ipc_fd,
                            error)) {
                return false;
            }
        } else if (arg == "--no-audio") {
            options->no_audio = true;
        } else if (arg == "--validate-only") {
            options->validate_only = true;
        } else {
            *error = "unknown option: " + arg;
            return false;
        }
    }
    return true;
}

bool LoadConfigFile(const std::string& path, WorkerConfig* config, std::string* error) {
    if (config == nullptr || error == nullptr) {
        return false;
    }
    if (path.empty()) {
        return true;
    }

    std::ifstream input(path);
    if (!input) {
        *error = "cannot open config file: " + path;
        return false;
    }

    try {
        json root;
        input >> root;
        if (!root.is_object()) {
            *error = "config root must be a JSON object";
            return false;
        }

        if (auto it = root.find("runtime"); it != root.end()) {
            ReadIfPresent(*it, "generation", &config->runtime.generation);
            ReadIfPresent(*it, "duration_sec", &config->runtime.duration_sec);
            ReadIfPresent(*it, "metrics_interval_ms", &config->runtime.metrics_interval_ms);
            ReadIfPresent(*it, "warning_timeout_count",
                          &config->runtime.warning_timeout_count);
            ReadIfPresent(*it, "stalled_timeout_count",
                          &config->runtime.stalled_timeout_count);
            ReadIfPresent(*it, "fatal_timeout_count", &config->runtime.fatal_timeout_count);
        }
        if (auto it = root.find("isp"); it != root.end()) {
            ReadIfPresent(*it, "iq_dir", &config->isp.iq_dir);
            ReadIfPresent(*it, "camera_id", &config->isp.camera_id);
        }
        if (auto it = root.find("vi"); it != root.end()) {
            ReadIfPresent(*it, "device_id", &config->vi.device_id);
            ReadIfPresent(*it, "pipe_id", &config->vi.pipe_id);
            ReadIfPresent(*it, "channel_id", &config->vi.channel_id);
            ReadIfPresent(*it, "buffer_count", &config->vi.buffer_count);
        }
        if (auto it = root.find("vpss"); it != root.end()) {
            ReadIfPresent(*it, "group_id", &config->vpss.group_id);
            ReadIfPresent(*it, "channel_id", &config->vpss.channel_id);
        }
        if (auto it = root.find("video"); it != root.end()) {
            ReadIfPresent(*it, "enabled", &config->video.enabled);
            ReadIfPresent(*it, "width", &config->video.width);
            ReadIfPresent(*it, "height", &config->video.height);
            ReadIfPresent(*it, "fps", &config->video.fps);
            ReadIfPresent(*it, "bitrate_kbps", &config->video.bitrate_kbps);
            ReadIfPresent(*it, "gop", &config->video.gop);
            ReadIfPresent(*it, "venc_channel_id", &config->video.venc_channel_id);
            ReadIfPresent(*it, "stream_buffer_count", &config->video.stream_buffer_count);
            ReadIfPresent(*it, "output_path", &config->video.output_path);
        }
        if (auto it = root.find("audio"); it != root.end()) {
            ReadIfPresent(*it, "enabled", &config->audio.enabled);
            ReadIfPresent(*it, "card_name", &config->audio.card_name);
            ReadIfPresent(*it, "device_id", &config->audio.device_id);
            ReadIfPresent(*it, "channel_id", &config->audio.channel_id);
            ReadIfPresent(*it, "aenc_channel_id", &config->audio.aenc_channel_id);
            ReadIfPresent(*it, "device_sample_rate", &config->audio.device_sample_rate);
            ReadIfPresent(*it, "sample_rate", &config->audio.sample_rate);
            ReadIfPresent(*it, "device_channels", &config->audio.device_channels);
            ReadIfPresent(*it, "channels", &config->audio.channels);
            ReadIfPresent(*it, "bit_width", &config->audio.bit_width);
            ReadIfPresent(*it, "frame_samples", &config->audio.frame_samples);
            ReadIfPresent(*it, "bitrate", &config->audio.bitrate);
            ReadIfPresent(*it, "buffer_count", &config->audio.buffer_count);
            ReadIfPresent(*it, "output_path", &config->audio.output_path);
        }
    } catch (const std::exception& exception) {
        *error = std::string("invalid config file: ") + exception.what();
        return false;
    }
    return true;
}

void ApplyCliOverrides(const CliOptions& options, WorkerConfig* config) {
    if (options.generation) config->runtime.generation = *options.generation;
    if (options.duration_sec) config->runtime.duration_sec = *options.duration_sec;
    if (options.width) config->video.width = *options.width;
    if (options.height) config->video.height = *options.height;
    if (options.fps) config->video.fps = *options.fps;
    if (options.bitrate_kbps) config->video.bitrate_kbps = *options.bitrate_kbps;
    if (options.gop) config->video.gop = *options.gop;
    if (options.video_output) config->video.output_path = *options.video_output;
    if (options.audio_output) config->audio.output_path = *options.audio_output;
    if (options.iq_dir) config->isp.iq_dir = *options.iq_dir;
    if (options.video_ipc_fd) config->video.ipc_fd = *options.video_ipc_fd;
    if (options.no_audio) config->audio.enabled = false;
}

std::vector<std::string> ValidateConfig(const WorkerConfig& config) {
    std::vector<std::string> errors;
    auto require_range = [&errors](const char* name, int value, int minimum, int maximum) {
        if (value < minimum || value > maximum) {
            std::ostringstream stream;
            stream << name << " must be in [" << minimum << ", " << maximum << "]";
            errors.push_back(stream.str());
        }
    };

    if (config.runtime.generation.empty()) errors.push_back("runtime.generation is required");
    require_range("runtime.duration_sec", config.runtime.duration_sec, 0, 86400);
    require_range("runtime.metrics_interval_ms", config.runtime.metrics_interval_ms, 250, 60000);
    if (!(config.runtime.warning_timeout_count < config.runtime.stalled_timeout_count &&
          config.runtime.stalled_timeout_count < config.runtime.fatal_timeout_count)) {
        errors.push_back("timeout counts must satisfy warning < stalled < fatal");
    }
    if (config.isp.iq_dir.empty()) errors.push_back("isp.iq_dir is required");

    if (!config.video.enabled) {
        errors.push_back("video.enabled must be true in media worker v1");
    }
    require_range("video.width", config.video.width, 160, 4096);
    require_range("video.height", config.video.height, 120, 4096);
    if (config.video.width % 2 != 0 || config.video.height % 2 != 0) {
        errors.push_back("video width and height must be even for NV12");
    }
    require_range("video.fps", config.video.fps, 1, 60);
    require_range("video.bitrate_kbps", config.video.bitrate_kbps, 64, 50000);
    require_range("video.gop", config.video.gop, 1, 300);
    require_range("vi.buffer_count", config.vi.buffer_count, 1, 16);
    require_range("video.stream_buffer_count", config.video.stream_buffer_count, 1, 16);
    if (config.video.ipc_fd != -1 &&
        (config.video.ipc_fd < 3 || config.video.ipc_fd > 1024)) {
        errors.push_back("video IPC fd must be -1 or in [3, 1024]");
    }

    const int channel_values[] = {config.isp.camera_id, config.vi.device_id,
                                  config.vi.pipe_id, config.vi.channel_id,
                                  config.vpss.group_id, config.vpss.channel_id,
                                  config.video.venc_channel_id};
    for (int value : channel_values) {
        if (value < 0 || value > 63) {
            errors.push_back("video hardware channel IDs must be in [0, 63]");
            break;
        }
    }

    if (config.audio.enabled) {
        if (config.audio.card_name.empty()) errors.push_back("audio.card_name is required");
        if (config.audio.sample_rate != 8000) {
            errors.push_back("audio.sample_rate must be 8000 for G711A v1");
        }
        if (config.audio.channels != 1) {
            errors.push_back("audio.channels must be 1 for G711A v1");
        }
        if (config.audio.bit_width != 16) {
            errors.push_back("audio.bit_width must be 16 for G711A v1");
        }
        require_range("audio.device_sample_rate", config.audio.device_sample_rate, 8000, 48000);
        require_range("audio.device_channels", config.audio.device_channels, 1, 2);
        require_range("audio.frame_samples", config.audio.frame_samples, 80, 2048);
        require_range("audio.buffer_count", config.audio.buffer_count, 2, 16);
        if (config.audio.bitrate != 64000) {
            errors.push_back("audio.bitrate must be 64000 for G711A v1");
        }
        if (config.audio.device_id < 0 || config.audio.channel_id < 0 ||
            config.audio.aenc_channel_id < 0) {
            errors.push_back("audio hardware channel IDs must be non-negative");
        }
    }
    return errors;
}

std::string Usage(const char* program_name) {
    std::ostringstream output;
    output << "Usage: " << program_name << " [options]\n"
           << "  --config <path>          JSON configuration file\n"
           << "  --generation <value>     Override generation\n"
           << "  --duration-sec <seconds> Stop automatically (0 means run forever)\n"
           << "  --width/--height <value> Override H264 resolution\n"
           << "  --fps <value>            Override frame rate\n"
           << "  --bitrate-kbps <value>   Override H264 CBR bitrate\n"
           << "  --gop <value>            Override H264 GOP\n"
           << "  --video-output <path>    Optional H264 diagnostic dump\n"
           << "  --audio-output <path>    Optional G711A diagnostic dump\n"
           << "  --iq-dir <path>          Override ISP IQ directory\n"
           << "  --video-ipc-fd <fd>      Publish framed H264 to inherited descriptor\n"
           << "  --no-audio               Disable AI/AENC pipeline\n"
           << "  --validate-only          Validate config without accessing hardware\n"
           << "  --help                   Show this help\n";
    return output.str();
}

}  // namespace media_worker
