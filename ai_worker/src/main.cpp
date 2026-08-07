#include <lua.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <unistd.h>

#if AIPC_ENABLE_VISIONG
#include <visiong/core/ImageBuffer.h>
#include <visiong/npu/NPU.h>
#endif

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr std::size_t kAipfHeaderSize = 88;
constexpr std::size_t kMaxFrameBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxResultBytes = 256 * 1024;

struct Options {
    fs::path project_dir;
    fs::path models_dir;
    int input_fd = 3;
    int output_fd = 4;
    bool validate_only = false;
    bool mock = false;
};

struct Frame {
    std::uint64_t pts = 0;
    std::uint64_t sequence = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t y_stride = 0;
    std::uint32_t uv_stride = 0;
    std::uint32_t height_stride = 0;
    std::vector<std::uint8_t> data;
};

struct DetectionResult {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    float score = 0.0F;
    int class_id = 0;
    std::string label;
};

struct Manifest {
    std::string id;
    std::string name;
    std::string entry = "main.lua";
    std::string algorithm = "yolov5";
    std::string model;
    std::string labels;
    float threshold = 0.25F;
    float nms_threshold = 0.45F;
    int max_detections = 32;
    std::vector<int> class_filter;
    json raw;
};

bool ReadAll(int fd, void* output, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(output);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t result = read(fd, bytes + offset, size - offset);
        if (result > 0) {
            offset += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool WriteAll(int fd, const void* input, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(input);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t result = write(fd, bytes + offset, size - offset);
        if (result > 0) {
            offset += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool WriteMessage(int fd, const json& value) {
    const std::string payload = value.dump();
    if (payload.empty() || payload.size() > kMaxResultBytes) return false;
    const std::uint32_t length = static_cast<std::uint32_t>(payload.size());
    const std::uint8_t header[4] = {
        static_cast<std::uint8_t>(length >> 24),
        static_cast<std::uint8_t>(length >> 16),
        static_cast<std::uint8_t>(length >> 8),
        static_cast<std::uint8_t>(length),
    };
    return WriteAll(fd, header, sizeof(header)) &&
           WriteAll(fd, payload.data(), payload.size());
}

std::uint32_t U32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

std::uint64_t U64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) value = (value << 8) | data[index];
    return value;
}

std::optional<Frame> ReadFrame(int fd, std::string* error) {
    std::uint8_t header[kAipfHeaderSize];
    if (!ReadAll(fd, header, sizeof(header))) return std::nullopt;
    if (std::memcmp(header, "AIPF", 4) != 0 || header[4] != 0 || header[5] != 1) {
        *error = "invalid AIPF header";
        return std::nullopt;
    }
    const std::size_t length = U32(header + 8);
    Frame frame;
    frame.pts = U64(header + 12);
    frame.sequence = U64(header + 20);
    frame.width = U32(header + 28);
    frame.height = U32(header + 32);
    frame.y_stride = U32(header + 36);
    frame.uv_stride = U32(header + 40);
    frame.height_stride = U32(header + 44);
    const std::size_t expected =
        static_cast<std::size_t>(frame.y_stride) * frame.height_stride * 3 / 2;
    if (length == 0 || length > kMaxFrameBytes || frame.width == 0 ||
        frame.height == 0 || frame.y_stride < frame.width ||
        frame.height_stride < frame.height || length < expected) {
        *error = "inconsistent AIPF dimensions or payload length";
        return std::nullopt;
    }
    frame.data.resize(length);
    if (!ReadAll(fd, frame.data.data(), frame.data.size())) {
        *error = "truncated AIPF payload";
        return std::nullopt;
    }
    return frame;
}

#if AIPC_ENABLE_VISIONG
std::vector<std::uint8_t> RepackNv12(const Frame& frame) {
    std::vector<std::uint8_t> packed(
        static_cast<std::size_t>(frame.width) * frame.height * 3 / 2);
    for (std::uint32_t row = 0; row < frame.height; ++row) {
        std::memcpy(packed.data() + static_cast<std::size_t>(row) * frame.width,
                    frame.data.data() + static_cast<std::size_t>(row) * frame.y_stride,
                    frame.width);
    }
    const std::size_t source_uv =
        static_cast<std::size_t>(frame.y_stride) * frame.height_stride;
    const std::size_t target_uv =
        static_cast<std::size_t>(frame.width) * frame.height;
    for (std::uint32_t row = 0; row < frame.height / 2; ++row) {
        std::memcpy(packed.data() + target_uv + static_cast<std::size_t>(row) * frame.width,
                    frame.data.data() + source_uv +
                        static_cast<std::size_t>(row) * frame.uv_stride,
                    frame.width);
    }
    return packed;
}
#endif

bool SafeName(const std::string& value) {
    return !value.empty() && value != "." && value != ".." &&
           value.find('/') == std::string::npos &&
           value.find('\\') == std::string::npos;
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
    if (!SafeName(manifest.id) || !SafeName(manifest.entry) || !SafeName(manifest.model) ||
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

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::vector<DetectionResult> Infer(const Frame& frame) = 0;
    virtual const char* Name() const = 0;
};

class MockBackend final : public Backend {
public:
    std::vector<DetectionResult> Infer(const Frame& frame) override {
        const int width = static_cast<int>(frame.width);
        const int height = static_cast<int>(frame.height);
        const int offset = static_cast<int>(frame.sequence % std::max(1, width / 4));
        return {{offset, height / 4, std::min(width - 1, offset + width / 3),
                 std::min(height - 1, height * 3 / 4), 0.9F, 0, "mock"}};
    }
    const char* Name() const override { return "mock"; }
};

#if AIPC_ENABLE_VISIONG
class VisionGBackend final : public Backend {
public:
    VisionGBackend(const fs::path& model, const fs::path& labels, float threshold,
                   float nms_threshold)
        : npu_(ModelType::YOLOV5, model.string(), labels.string(), threshold,
               nms_threshold) {
        if (!npu_.is_initialized()) throw std::runtime_error("VisionG NPU initialization failed");
    }

    std::vector<DetectionResult> Infer(const Frame& frame) override {
        ImageBuffer image(static_cast<int>(frame.width), static_cast<int>(frame.height),
                          RK_FMT_YUV420SP, RepackNv12(frame));
        const auto detections = npu_.infer(image, {0, 0, 0, 0}, "rgb");
        std::vector<DetectionResult> output;
        output.reserve(detections.size());
        for (const auto& detection : detections) {
            const auto [x1, y1, x2, y2] = detection.box;
            output.push_back(
                {x1, y1, x2, y2, detection.score, detection.class_id, detection.label});
        }
        return output;
    }

    const char* Name() const override { return "visiong"; }

private:
    NPU npu_;
};
#endif

class LuaRuntime {
public:
    LuaRuntime(const Manifest& manifest, const fs::path& project_dir,
               std::unique_ptr<Backend> backend)
        : manifest_(manifest), project_dir_(project_dir), backend_(std::move(backend)) {
        state_ = luaL_newstate();
        if (!state_) throw std::runtime_error("cannot allocate Lua state");
        luaL_openlibs(state_);
        InstallSandbox();
        InstallAipcModule();
        const fs::path entry = project_dir_ / manifest_.entry;
        if (luaL_loadfile(state_, entry.c_str()) != LUA_OK) {
            throw std::runtime_error(PopError("Lua load failed"));
        }
        ProtectedCall(0, 0);
        lua_getglobal(state_, "process");
        const bool has_process = lua_isfunction(state_, -1);
        lua_pop(state_, 1);
        if (!has_process) throw std::runtime_error("Lua project must define process(frame)");
        lua_getglobal(state_, "init");
        if (lua_isfunction(state_, -1)) {
            JsonToLua(manifest_.raw, 0);
            ProtectedCall(1, 0);
        } else {
            lua_pop(state_, 1);
        }
    }

    ~LuaRuntime() {
        if (!state_) return;
        lua_getglobal(state_, "shutdown");
        if (lua_isfunction(state_, -1)) {
            try {
                ProtectedCall(0, 0);
            } catch (const std::exception& error) {
                std::cerr << "Lua shutdown warning: " << error.what() << '\n';
            }
        } else {
            lua_pop(state_, 1);
        }
        lua_close(state_);
    }

    json Process(const Frame& frame) {
        current_frame_ = &frame;
        lua_getglobal(state_, "process");
        lua_createtable(state_, 0, 5);
        SetInteger("sequence", frame.sequence);
        SetInteger("pts", frame.pts);
        SetInteger("width", frame.width);
        SetInteger("height", frame.height);
        SetString("format", "nv12");
        try {
            ProtectedCall(1, 1);
            json result = LuaToJson(-1, 0);
            lua_pop(state_, 1);
            current_frame_ = nullptr;
            if (result.is_object() && result.empty()) result = json::array();
            if (!result.is_array()) throw std::runtime_error("process(frame) must return an array");
            if (result.size() > static_cast<std::size_t>(manifest_.max_detections)) {
                result.erase(result.begin() + manifest_.max_detections, result.end());
            }
            return result;
        } catch (...) {
            current_frame_ = nullptr;
            throw;
        }
    }

    const char* BackendName() const { return backend_->Name(); }

private:
    static LuaRuntime* Self(lua_State* state) {
        return static_cast<LuaRuntime*>(lua_touserdata(state, lua_upvalueindex(1)));
    }

    static int LoadModel(lua_State* state) {
        auto* self = Self(state);
        const char* value = luaL_optstring(state, 1, self->manifest_.model.c_str());
        if (value != self->manifest_.model) return luaL_error(state, "model is fixed by manifest");
        lua_pushinteger(state, 1);
        return 1;
    }

    static int Detect(lua_State* state) {
        auto* self = Self(state);
        if (!self->current_frame_) return luaL_error(state, "detect() called outside process()");
        try {
            auto detections = self->backend_->Infer(*self->current_frame_);
            if (!self->manifest_.class_filter.empty()) {
                detections.erase(
                    std::remove_if(detections.begin(), detections.end(),
                                   [self](const DetectionResult& detection) {
                                       return std::find(self->manifest_.class_filter.begin(),
                                                        self->manifest_.class_filter.end(),
                                                        detection.class_id) ==
                                              self->manifest_.class_filter.end();
                                   }),
                    detections.end());
            }
            if (detections.size() > static_cast<std::size_t>(self->manifest_.max_detections)) {
                detections.resize(self->manifest_.max_detections);
            }
            lua_createtable(state, static_cast<int>(detections.size()), 0);
            int index = 1;
            for (const auto& detection : detections) {
                lua_createtable(state, 0, 7);
                lua_pushinteger(state, detection.x1);
                lua_setfield(state, -2, "x1");
                lua_pushinteger(state, detection.y1);
                lua_setfield(state, -2, "y1");
                lua_pushinteger(state, detection.x2);
                lua_setfield(state, -2, "x2");
                lua_pushinteger(state, detection.y2);
                lua_setfield(state, -2, "y2");
                lua_pushnumber(state, detection.score);
                lua_setfield(state, -2, "confidence");
                lua_pushinteger(state, detection.class_id);
                lua_setfield(state, -2, "class_id");
                lua_pushlstring(state, detection.label.data(), detection.label.size());
                lua_setfield(state, -2, "label");
                lua_rawseti(state, -2, index++);
            }
            return 1;
        } catch (const std::exception& error) {
            return luaL_error(state, "%s", error.what());
        }
    }

    static int FrameInfo(lua_State* state) {
        auto* self = Self(state);
        if (!self->current_frame_) return luaL_error(state, "frame_info outside process()");
        lua_createtable(state, 0, 4);
        lua_pushinteger(state, self->current_frame_->sequence);
        lua_setfield(state, -2, "sequence");
        lua_pushinteger(state, self->current_frame_->pts);
        lua_setfield(state, -2, "pts");
        lua_pushinteger(state, self->current_frame_->width);
        lua_setfield(state, -2, "width");
        lua_pushinteger(state, self->current_frame_->height);
        lua_setfield(state, -2, "height");
        return 1;
    }

    static int Log(lua_State* state) {
        const char* level = luaL_optstring(state, 1, "info");
        const char* message = luaL_checkstring(state, 2);
        std::cerr << "lua[" << level << "]: " << message << '\n';
        return 0;
    }

    static void InstructionLimit(lua_State* state, lua_Debug*) {
        luaL_error(state, "Lua instruction limit exceeded");
    }

    void InstallSandbox() {
        for (const char* name : {"os", "io", "package", "debug", "require", "dofile",
                                 "loadfile"}) {
            lua_pushnil(state_);
            lua_setglobal(state_, name);
        }
    }

    void InstallAipcModule() {
        lua_createtable(state_, 0, 4);
        for (const auto& method :
             std::vector<std::pair<const char*, lua_CFunction>>{
                 {"load_model", LoadModel}, {"detect", Detect},
                 {"frame_info", FrameInfo}, {"log", Log}}) {
            lua_pushlightuserdata(state_, this);
            lua_pushcclosure(state_, method.second, 1);
            lua_setfield(state_, -2, method.first);
        }
        lua_setglobal(state_, "aipc");
    }

    void ProtectedCall(int arguments, int results) {
        lua_sethook(state_, InstructionLimit, LUA_MASKCOUNT, 1'000'000);
        const int status = lua_pcall(state_, arguments, results, 0);
        lua_sethook(state_, nullptr, 0, 0);
        if (status != LUA_OK) throw std::runtime_error(PopError("Lua execution failed"));
    }

    std::string PopError(const char* prefix) {
        const char* value = lua_tostring(state_, -1);
        std::string result = std::string(prefix) + ": " + (value ? value : "unknown error");
        lua_pop(state_, 1);
        return result;
    }

    void SetInteger(const char* key, std::uint64_t value) {
        lua_pushinteger(state_, static_cast<lua_Integer>(value));
        lua_setfield(state_, -2, key);
    }

    void SetString(const char* key, const char* value) {
        lua_pushstring(state_, value);
        lua_setfield(state_, -2, key);
    }

    void JsonToLua(const json& value, int depth) {
        if (depth > 8) throw std::runtime_error("manifest nesting exceeds Lua limit");
        if (value.is_null()) {
            lua_pushnil(state_);
        } else if (value.is_boolean()) {
            lua_pushboolean(state_, value.get<bool>());
        } else if (value.is_number_integer()) {
            lua_pushinteger(state_, value.get<lua_Integer>());
        } else if (value.is_number()) {
            lua_pushnumber(state_, value.get<lua_Number>());
        } else if (value.is_string()) {
            const auto text = value.get<std::string>();
            lua_pushlstring(state_, text.data(), text.size());
        } else if (value.is_array()) {
            lua_createtable(state_, static_cast<int>(value.size()), 0);
            int index = 1;
            for (const auto& item : value) {
                JsonToLua(item, depth + 1);
                lua_rawseti(state_, -2, index++);
            }
        } else {
            lua_createtable(state_, 0, static_cast<int>(value.size()));
            for (auto item = value.begin(); item != value.end(); ++item) {
                JsonToLua(item.value(), depth + 1);
                lua_setfield(state_, -2, item.key().c_str());
            }
        }
    }

    json LuaToJson(int index, int depth) {
        if (depth > 8) throw std::runtime_error("Lua result nesting exceeds limit");
        index = lua_absindex(state_, index);
        switch (lua_type(state_, index)) {
            case LUA_TNIL:
                return nullptr;
            case LUA_TBOOLEAN:
                return lua_toboolean(state_, index) != 0;
            case LUA_TNUMBER:
                if (lua_isinteger(state_, index)) return lua_tointeger(state_, index);
                return lua_tonumber(state_, index);
            case LUA_TSTRING: {
                std::size_t length = 0;
                const char* value = lua_tolstring(state_, index, &length);
                if (length > 4096) throw std::runtime_error("Lua result string too long");
                return std::string(value, length);
            }
            case LUA_TTABLE: {
                const lua_Integer length = luaL_len(state_, index);
                if (length > 0) {
                    json output = json::array();
                    for (lua_Integer item = 1; item <= length; ++item) {
                        lua_rawgeti(state_, index, item);
                        output.push_back(LuaToJson(-1, depth + 1));
                        lua_pop(state_, 1);
                    }
                    return output;
                }
                json output = json::object();
                lua_pushnil(state_);
                while (lua_next(state_, index) != 0) {
                    if (lua_type(state_, -2) != LUA_TSTRING) {
                        lua_pop(state_, 1);
                        throw std::runtime_error("Lua object keys must be strings");
                    }
                    const char* key = lua_tostring(state_, -2);
                    output[key] = LuaToJson(-1, depth + 1);
                    lua_pop(state_, 1);
                    if (output.size() > 64) throw std::runtime_error("Lua object too large");
                }
                return output;
            }
            default:
                throw std::runtime_error("unsupported Lua result type");
        }
    }

    Manifest manifest_;
    fs::path project_dir_;
    std::unique_ptr<Backend> backend_;
    lua_State* state_ = nullptr;
    const Frame* current_frame_ = nullptr;
};

Options ParseOptions(int argc, char* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value = [&](const char* name) {
            if (++index >= argc) throw std::runtime_error(std::string("missing value for ") + name);
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
        } else {
            throw std::runtime_error("unknown option: " + argument);
        }
    }
    if (options.project_dir.empty() || options.models_dir.empty()) {
        throw std::runtime_error("--project-dir and --models-dir are required");
    }
    return options;
}

std::unique_ptr<Backend> CreateBackend(const Options& options, const Manifest& manifest) {
    if (options.mock) return std::make_unique<MockBackend>();
#if AIPC_ENABLE_VISIONG
    const fs::path model = options.models_dir / manifest.model;
    const fs::path labels = options.models_dir / manifest.labels;
    if (!fs::is_regular_file(model)) throw std::runtime_error("model file does not exist");
    if (!manifest.labels.empty() && !fs::is_regular_file(labels)) {
        throw std::runtime_error("labels file does not exist");
    }
    return std::make_unique<VisionGBackend>(model, labels, manifest.threshold,
                                            manifest.nms_threshold);
#else
    (void)manifest;
    throw std::runtime_error("ai_worker was built without VisionG; use --mock for tests");
#endif
}

int Run(int argc, char* argv[]) {
    const Options options = ParseOptions(argc, argv);
    const Manifest manifest = LoadManifest(options.project_dir);
    if (!fs::is_regular_file(options.project_dir / manifest.entry)) {
        throw std::runtime_error("Lua entry file does not exist");
    }
    if (!options.mock && !fs::is_regular_file(options.models_dir / manifest.model)) {
        throw std::runtime_error("model file does not exist");
    }
    if (options.validate_only) {
        LuaRuntime runtime(manifest, options.project_dir, std::make_unique<MockBackend>());
        std::cout << json{{"valid", true}, {"project", manifest.id}}.dump() << '\n';
        return 0;
    }
    auto backend = CreateBackend(options, manifest);
    LuaRuntime runtime(manifest, options.project_dir, std::move(backend));
    if (!WriteMessage(options.output_fd,
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
        auto frame = ReadFrame(options.input_fd, &error);
        if (!frame) {
            if (!error.empty()) throw std::runtime_error(error);
            break;
        }
        const auto started = std::chrono::steady_clock::now();
        try {
            json detections = runtime.Process(*frame);
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            if (!WriteMessage(options.output_fd,
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
            WriteMessage(options.output_fd,
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

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return Run(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr << "ai_worker fatal: " << exception.what() << '\n';
        return 2;
    }
}
