#include "config.h"

#include <cstdlib>

#include <fstream>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

void TestDefaults() {
    media_worker::WorkerConfig config;
    Expect(config.video.width == 1920, "default width");
    Expect(config.audio.enabled, "audio enabled by default");
    Expect(!config.ai_input.enabled, "AI input disabled until a manifest is active");
    Expect(config.ai_input.channel_id == 1, "AI channel defaults to one");
    Expect(media_worker::ValidateConfig(config).empty(), "default config validates");
}

void TestCliOverrides() {
    const char* raw[] = {"media_worker", "--width", "1280", "--fps", "25",
                         "--generation", "test-2", "--no-audio"};
    auto argv = const_cast<char**>(raw);
    media_worker::CliOptions options;
    std::string error;
    Expect(media_worker::ParseCli(8, argv, &options, &error), "CLI parses");
    media_worker::WorkerConfig config;
    media_worker::ApplyCliOverrides(options, &config);
    Expect(config.video.width == 1280, "width override");
    Expect(config.video.fps == 25, "fps override");
    Expect(config.runtime.generation == "test-2", "generation override");
    Expect(!config.audio.enabled, "no-audio override");
}

void TestJsonAndValidation() {
    const std::string path = "/tmp/media_worker_config_test.json";
    {
        std::ofstream output(path);
        output << R"({
          "runtime": {"generation": "json-generation", "duration_sec": 12},
          "video": {"width": 1280, "height": 720, "fps": 20},
          "audio": {"enabled": false}
        })";
    }
    media_worker::WorkerConfig config;
    std::string error;
    Expect(media_worker::LoadConfigFile(path, &config, &error), "JSON config loads");
    Expect(config.runtime.generation == "json-generation", "JSON generation");
    Expect(config.runtime.duration_sec == 12, "JSON duration");
    Expect(config.video.width == 1280 && config.video.height == 720, "JSON resolution");

    config.video.width = 1279;
    Expect(!media_worker::ValidateConfig(config).empty(), "odd NV12 width rejected");
    std::remove(path.c_str());
}

void TestInvalidCli() {
    const char* raw[] = {"media_worker", "--fps", "fast"};
    auto argv = const_cast<char**>(raw);
    media_worker::CliOptions options;
    std::string error;
    Expect(!media_worker::ParseCli(3, argv, &options, &error), "invalid integer rejected");
}

void TestAiInputValidation() {
    media_worker::WorkerConfig config;
    config.ai_input.enabled = true;
    config.ai_input.width = 640;
    config.ai_input.height = 640;
    Expect(media_worker::ValidateConfig(config).empty(), "640x640 AI input validates");
    config.ai_input.channel_id = config.vpss.channel_id;
    Expect(!media_worker::ValidateConfig(config).empty(), "AI/main VPSS collision rejected");
    config.ai_input.channel_id = 1;
    config.ai_input.fit_mode = "invalid";
    Expect(!media_worker::ValidateConfig(config).empty(), "invalid fit mode rejected");
    config.ai_input.fit_mode = "contain";
    config.ai_input.width = 320;
    Expect(!media_worker::ValidateConfig(config).empty(), "unsafe small AI width rejected");
    config.ai_input.width = 640;
    config.ai_input.height = 240;
    Expect(!media_worker::ValidateConfig(config).empty(), "unsafe small AI height rejected");
}

void TestValidateOnlyCli() {
    const char* raw[] = {"media_worker", "--validate-only"};
    auto argv = const_cast<char**>(raw);
    media_worker::CliOptions options;
    std::string error;
    Expect(media_worker::ParseCli(2, argv, &options, &error), "validate-only parses");
    Expect(options.validate_only, "validate-only flag set");
}

void TestVideoIpcFdCli() {
    const char* raw[] = {"media_worker", "--video-ipc-fd", "3"};
    auto argv = const_cast<char**>(raw);
    media_worker::CliOptions options;
    std::string error;
    Expect(media_worker::ParseCli(3, argv, &options, &error), "video IPC fd parses");
    media_worker::WorkerConfig config;
    media_worker::ApplyCliOverrides(options, &config);
    Expect(config.video.ipc_fd == 3, "video IPC fd applied");
    Expect(media_worker::ValidateConfig(config).empty(), "video IPC fd validates");
    config.video.ipc_fd = 2;
    Expect(!media_worker::ValidateConfig(config).empty(), "reserved IPC fd rejected");
}

void TestAudioIpcFdCli() {
    const char* raw[] = {"media_worker", "--audio-ipc-fd", "4"};
    auto argv = const_cast<char**>(raw);
    media_worker::CliOptions options;
    std::string error;
    Expect(media_worker::ParseCli(3, argv, &options, &error), "audio IPC fd parses");
    media_worker::WorkerConfig config;
    media_worker::ApplyCliOverrides(options, &config);
    Expect(config.audio.ipc_fd == 4, "audio IPC fd applied");
    Expect(media_worker::ValidateConfig(config).empty(), "audio IPC fd validates");
    config.audio.ipc_fd = 2;
    Expect(!media_worker::ValidateConfig(config).empty(), "reserved audio IPC fd rejected");
}

void TestInvalidJson() {
    const std::string path = "/tmp/media_worker_invalid_config_test.json";
    {
        std::ofstream output(path);
        output << "{invalid-json";
    }
    media_worker::WorkerConfig config;
    std::string error;
    Expect(!media_worker::LoadConfigFile(path, &config, &error), "invalid JSON rejected");
    std::remove(path.c_str());
}

}  // namespace

int main() {
    TestDefaults();
    TestCliOverrides();
    TestJsonAndValidation();
    TestInvalidCli();
    TestAiInputValidation();
    TestValidateOnlyCli();
    TestVideoIpcFdCli();
    TestAudioIpcFdCli();
    TestInvalidJson();
    if (g_failures != 0) {
        std::cerr << g_failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all media_worker config tests passed\n";
    return EXIT_SUCCESS;
}
