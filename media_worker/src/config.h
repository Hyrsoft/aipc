#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace media_worker {

struct RuntimeConfig {
    std::string generation = "standalone-1";
    int duration_sec = 0;
    int metrics_interval_ms = 5000;
    int warning_timeout_count = 3;
    int stalled_timeout_count = 10;
    int fatal_timeout_count = 30;
};

struct IspConfig {
    std::string iq_dir = "/etc/iqfiles";
    int camera_id = 0;
};

struct ViConfig {
    int device_id = 0;
    int pipe_id = 0;
    int channel_id = 0;
    int buffer_count = 2;
};

struct VpssConfig {
    int group_id = 0;
    int channel_id = 0;
};

struct VideoConfig {
    bool enabled = true;
    int width = 1920;
    int height = 1080;
    int fps = 30;
    int bitrate_kbps = 4096;
    int gop = 30;
    int venc_channel_id = 0;
    int stream_buffer_count = 3;
    std::string output_path = "/tmp/media_worker_video.h264";
    int ipc_fd = -1;
};

struct AudioConfig {
    bool enabled = true;
    std::string card_name = "hw:0,0";
    int device_id = 0;
    int channel_id = 0;
    int aenc_channel_id = 0;
    int device_sample_rate = 8000;
    int sample_rate = 8000;
    int device_channels = 2;
    int channels = 1;
    int bit_width = 16;
    int frame_samples = 1024;
    int bitrate = 64000;
    int buffer_count = 4;
    std::string output_path = "/tmp/media_worker_audio.g711a";
};

struct WorkerConfig {
    RuntimeConfig runtime;
    IspConfig isp;
    ViConfig vi;
    VpssConfig vpss;
    VideoConfig video;
    AudioConfig audio;
};

struct CliOptions {
    std::string config_path;
    std::optional<std::string> generation;
    std::optional<int> duration_sec;
    std::optional<int> width;
    std::optional<int> height;
    std::optional<int> fps;
    std::optional<int> bitrate_kbps;
    std::optional<int> gop;
    std::optional<std::string> video_output;
    std::optional<std::string> audio_output;
    std::optional<std::string> iq_dir;
    std::optional<int> video_ipc_fd;
    bool no_audio = false;
    bool validate_only = false;
    bool help = false;
};

bool ParseCli(int argc, char* argv[], CliOptions* options, std::string* error);
bool LoadConfigFile(const std::string& path, WorkerConfig* config, std::string* error);
void ApplyCliOverrides(const CliOptions& options, WorkerConfig* config);
std::vector<std::string> ValidateConfig(const WorkerConfig& config);
std::string Usage(const char* program_name);

}  // namespace media_worker
