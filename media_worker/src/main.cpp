#include <csignal>

#include <iostream>
#include <string>

#include "config.h"
#include "event_emitter.h"
#include "media_worker.h"

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void SignalHandler(int) {
    g_stop_requested = 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    media_worker::CliOptions options;
    std::string error;
    if (!media_worker::ParseCli(argc, argv, &options, &error)) {
        media_worker::EventEmitter events("unknown");
        events.Emit("FatalError", {{"stage", "cli"}, {"message", error}});
        std::cerr << media_worker::Usage(argv[0]);
        return static_cast<int>(media_worker::ExitCode::kConfigError);
    }
    if (options.help) {
        std::cerr << media_worker::Usage(argv[0]);
        return 0;
    }
    if (options.probe_load) {
        std::cout << R"({"loaded":true,"worker":"media_worker"})" << '\n';
        return 0;
    }

    media_worker::WorkerConfig config;
    if (!media_worker::LoadConfigFile(options.config_path, &config, &error)) {
        media_worker::EventEmitter events(config.runtime.generation);
        events.Emit("FatalError", {{"stage", "config_load"}, {"message", error}});
        return static_cast<int>(media_worker::ExitCode::kConfigError);
    }
    media_worker::ApplyCliOverrides(options, &config);
    const auto errors = media_worker::ValidateConfig(config);
    if (!errors.empty()) {
        media_worker::EventEmitter events(config.runtime.generation);
        events.Emit("FatalError", {{"stage", "config_validation"}, {"errors", errors}});
        return static_cast<int>(media_worker::ExitCode::kConfigError);
    }
    if (options.validate_only) {
        media_worker::EventEmitter events(config.runtime.generation);
        events.Emit("Stopped", {{"reason", "validation_only"}, {"exit_code", 0}});
        return 0;
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGPIPE, SIG_IGN);

    media_worker::MediaWorker worker(std::move(config));
    return worker.Run([] { return g_stop_requested != 0; });
}
