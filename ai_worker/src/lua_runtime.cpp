#include "lua_runtime.h"

#include <lua.hpp>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ai_worker {

struct LuaRuntime::Impl {
    Impl(const Manifest& manifest, const fs::path& project_dir,
         std::unique_ptr<Backend> backend)
        : manifest_(manifest),
          project_dir_(project_dir),
          backend_(std::move(backend)) {
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
        if (!has_process) {
            throw std::runtime_error("Lua project must define process(frame)");
        }
        lua_getglobal(state_, "init");
        if (lua_isfunction(state_, -1)) {
            JsonToLua(manifest_.raw, 0);
            ProtectedCall(1, 0);
        } else {
            lua_pop(state_, 1);
        }
    }

    ~Impl() {
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
            if (!result.is_array()) {
                throw std::runtime_error("process(frame) must return an array");
            }
            if (result.size() >
                static_cast<std::size_t>(manifest_.max_detections)) {
                result.erase(result.begin() + manifest_.max_detections,
                             result.end());
            }
            return result;
        } catch (...) {
            current_frame_ = nullptr;
            throw;
        }
    }

    const char* BackendName() const { return backend_->Name(); }

private:
    static Impl* Self(lua_State* state) {
        return static_cast<Impl*>(
            lua_touserdata(state, lua_upvalueindex(1)));
    }

    static int LoadModel(lua_State* state) {
        auto* self = Self(state);
        const char* value =
            luaL_optstring(state, 1, self->manifest_.model.c_str());
        if (value != self->manifest_.model) {
            return luaL_error(state, "model is fixed by manifest");
        }
        lua_pushinteger(state, 1);
        return 1;
    }

    static int Detect(lua_State* state) {
        auto* self = Self(state);
        if (!self->current_frame_) {
            return luaL_error(state, "detect() called outside process()");
        }
        try {
            auto detections = self->backend_->Infer(*self->current_frame_);
            if (!self->manifest_.class_filter.empty()) {
                detections.erase(
                    std::remove_if(
                        detections.begin(), detections.end(),
                        [self](const DetectionResult& detection) {
                            return std::find(
                                       self->manifest_.class_filter.begin(),
                                       self->manifest_.class_filter.end(),
                                       detection.class_id) ==
                                   self->manifest_.class_filter.end();
                        }),
                    detections.end());
            }
            if (detections.size() >
                static_cast<std::size_t>(self->manifest_.max_detections)) {
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
                lua_pushlstring(state, detection.label.data(),
                                detection.label.size());
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
        if (!self->current_frame_) {
            return luaL_error(state, "frame_info outside process()");
        }
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
        for (const char* name : {"os", "io", "package", "debug", "require",
                                 "dofile", "loadfile"}) {
            lua_pushnil(state_);
            lua_setglobal(state_, name);
        }
    }

    void InstallAipcModule() {
        lua_createtable(state_, 0, 4);
        for (const auto& method :
             std::vector<std::pair<const char*, lua_CFunction>>{
                 {"load_model", LoadModel},
                 {"detect", Detect},
                 {"frame_info", FrameInfo},
                 {"log", Log}}) {
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
        if (status != LUA_OK) {
            throw std::runtime_error(PopError("Lua execution failed"));
        }
    }

    std::string PopError(const char* prefix) {
        const char* value = lua_tostring(state_, -1);
        std::string result =
            std::string(prefix) + ": " + (value ? value : "unknown error");
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
        if (depth > 8) {
            throw std::runtime_error("manifest nesting exceeds Lua limit");
        }
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
        if (depth > 8) {
            throw std::runtime_error("Lua result nesting exceeds limit");
        }
        index = lua_absindex(state_, index);
        switch (lua_type(state_, index)) {
            case LUA_TNIL:
                return nullptr;
            case LUA_TBOOLEAN:
                return lua_toboolean(state_, index) != 0;
            case LUA_TNUMBER:
                if (lua_isinteger(state_, index)) {
                    return lua_tointeger(state_, index);
                }
                return lua_tonumber(state_, index);
            case LUA_TSTRING: {
                std::size_t length = 0;
                const char* value = lua_tolstring(state_, index, &length);
                if (length > 4096) {
                    throw std::runtime_error("Lua result string too long");
                }
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
                        throw std::runtime_error(
                            "Lua object keys must be strings");
                    }
                    const char* key = lua_tostring(state_, -2);
                    output[key] = LuaToJson(-1, depth + 1);
                    lua_pop(state_, 1);
                    if (output.size() > 64) {
                        throw std::runtime_error("Lua object too large");
                    }
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

LuaRuntime::LuaRuntime(const Manifest& manifest, const fs::path& project_dir,
                       std::unique_ptr<Backend> backend)
    : impl_(std::make_unique<Impl>(manifest, project_dir, std::move(backend))) {}

LuaRuntime::~LuaRuntime() = default;

json LuaRuntime::Process(const Frame& frame) { return impl_->Process(frame); }

const char* LuaRuntime::BackendName() const { return impl_->BackendName(); }

}  // namespace ai_worker
