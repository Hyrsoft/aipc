#define LOG_TAG "VisionGProd"

#include "visiong_producer.h"

#include "common/asio_context.h"
#include "common/logger.h"
#include "common/media_buffer.h"

#include "visiong/core/ImageBuffer.h"
#include "visiong/modules/VencManager.h"

#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <cstring>
#include <ctime>
#include <optional>
#include <utility>
#include <vector>

#include <pybind11/embed.h>

namespace py = pybind11;

namespace media {

namespace {

class SerialStreamDispatcher {
public:
    struct ConsumerInfo {
        std::string name;
        StreamCallback callback;
        StreamConsumerType type;
    };

    void RegisterConsumer(const std::string& name, StreamCallback callback, StreamConsumerType type) {
        consumers_.push_back({name, std::move(callback), type});
        LOG_INFO("Registered stream consumer: {}", name);
    }

    void ClearConsumers() {
        consumers_.clear();
    }

    void DispatchFrame(const EncodedStreamPtr& stream) {
        for (auto& c : consumers_) {
            if (!c.callback) {
                continue;
            }
            if (c.type == StreamConsumerType::AsyncIO) {
                PostToIo([callback = c.callback, stream]() { callback(stream); });
            } else {
                c.callback(stream);
            }
        }
    }

private:
    std::vector<ConsumerInfo> consumers_;
};

struct VisionGPacketOpaque {
    std::vector<unsigned char> payload;
};

static RK_S32 ReleaseVisionGPacketOpaque(void* opaque) {
    delete static_cast<VisionGPacketOpaque*>(opaque);
    return RK_SUCCESS;
}

static RK_U64 GetNowUs() {
    struct timespec time = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return static_cast<RK_U64>(time.tv_sec) * 1000000 + static_cast<RK_U64>(time.tv_nsec) / 1000;
}

static EncodedStreamPtr ConvertPacketToEncodedStream(const VencEncodedPacket& packet) {
    if (packet.data.empty()) {
        return nullptr;
    }

    auto* stream = new VENC_STREAM_S();
    auto* pack = new VENC_PACK_S();
    memset(stream, 0, sizeof(VENC_STREAM_S));
    memset(pack, 0, sizeof(VENC_PACK_S));
    stream->pstPack = pack;

    auto* opaque = new VisionGPacketOpaque();
    opaque->payload = packet.data;

    MB_EXT_CONFIG_S ext_cfg;
    memset(&ext_cfg, 0, sizeof(ext_cfg));
    ext_cfg.pu8VirAddr = opaque->payload.data();
    ext_cfg.u64PhyAddr = 0;
    ext_cfg.s32Fd = -1;
    ext_cfg.u64Size = opaque->payload.size();
    ext_cfg.pFreeCB = ReleaseVisionGPacketOpaque;
    ext_cfg.pOpaque = opaque;

    MB_BLK mb_blk = MB_INVALID_HANDLE;
    if (RK_MPI_SYS_CreateMB(&mb_blk, &ext_cfg) != RK_SUCCESS || mb_blk == MB_INVALID_HANDLE) {
        LOG_ERROR("RK_MPI_SYS_CreateMB failed while converting VisionG packet (size={})", packet.data.size());
        delete opaque;
        delete pack;
        delete stream;
        return nullptr;
    }

    stream->u32Seq = packet.stream_seq;
    stream->u32PackCount = 1;

    pack->pMbBlk = mb_blk;
    pack->u32Len = static_cast<RK_U32>(packet.data.size());
    pack->u64PTS = GetNowUs();
    pack->DataType.enH264EType = packet.is_keyframe ? H264E_NALU_IDRSLICE : H264E_NALU_PSLICE;

    return EncodedStreamPtr(stream, [](VENC_STREAM_S* p) {
        if (!p) {
            return;
        }
        if (p->pstPack && p->pstPack->pMbBlk != MB_INVALID_HANDLE) {
            RK_MPI_MB_ReleaseMB(p->pstPack->pMbBlk);
            p->pstPack->pMbBlk = MB_INVALID_HANDLE;
        }
        delete p->pstPack;
        delete p;
    });
}

static const char* kDefaultVisionGScript = R"PY(
def init():
    return None


def process():
    return None


def cleanup():
    return None
)PY";

void EnsureEmbeddedPythonReady() {
    static std::once_flag init_once;
    std::call_once(init_once, []() {
        LOG_INFO("[PythonInit] initialize_interpreter begin");
        py::initialize_interpreter(false, 0, nullptr, false);
        LOG_INFO("[PythonInit] initialize_interpreter done");
        py::module sys = py::module::import("sys");
        auto path = sys.attr("path").cast<py::list>();
        path.append("../python");
        LOG_INFO("[PythonInit] sys.path updated, Embedded Python interpreter initialized");
    });
}

class PythonRuntime {
public:
    PythonRuntime() {
        LOG_INFO("[PythonInit] PythonRuntime ctor begin");
        EnsureEmbeddedPythonReady();
        LOG_INFO("[PythonInit] PythonRuntime ctor after EnsureEmbeddedPythonReady");
        py::gil_scoped_acquire gil;
        LOG_INFO("[PythonInit] PythonRuntime ctor acquired GIL");
        globals_ = py::dict();
        process_fn_ = py::none();
        init_fn_ = py::none();
        cleanup_fn_ = py::none();
        LOG_INFO("[PythonInit] PythonRuntime ctor finished");
    }

    ~PythonRuntime() {
        Shutdown();
    }

    std::string LoadCode(const std::string& code) {
        std::lock_guard<std::mutex> lock(mutex_);
        py::gil_scoped_acquire gil;
        LOG_INFO("[PythonInit] LoadCode begin");

        try {
            if (!cleanup_fn_.is_none()) {
                LOG_INFO("[PythonInit] LoadCode calling previous cleanup()");
                cleanup_fn_();
            }

            py::dict globals;
            globals["__builtins__"] = py::module::import("builtins");
            LOG_INFO("[PythonInit] LoadCode exec begin");
            py::exec(code, globals);
            LOG_INFO("[PythonInit] LoadCode exec done");

            py::object process_fn = globals.contains("process") ? globals["process"] : py::none();
            if (process_fn.is_none() || !py::isinstance<py::function>(process_fn)) {
                return "Python code must define callable function: process()";
            }

            py::object init_fn = globals.contains("init") ? globals["init"] : py::none();
            py::object cleanup_fn = globals.contains("cleanup") ? globals["cleanup"] : py::none();

            globals_ = std::move(globals);
            process_fn_ = std::move(process_fn);
            init_fn_ = std::move(init_fn);
            cleanup_fn_ = std::move(cleanup_fn);

            if (!init_fn_.is_none()) {
                LOG_INFO("[PythonInit] LoadCode calling init()");
                init_fn_();
                LOG_INFO("[PythonInit] LoadCode init() done");
            }

            code_ = code;
            last_error_.clear();
            LOG_INFO("[PythonInit] LoadCode finished");
            return "";
        } catch (const std::exception& e) {
            last_error_ = e.what();
            LOG_ERROR("[PythonInit] LoadCode exception: {}", last_error_);
            return last_error_;
        }
    }

    std::optional<ImageBuffer> ProcessFrame() {
        std::lock_guard<std::mutex> lock(mutex_);
        py::gil_scoped_acquire gil;

        try {
            if (process_fn_.is_none()) {
                last_error_ = "process() function is not ready";
                return std::nullopt;
            }

            py::object result = process_fn_();
            if (result.is_none()) {
                return std::nullopt;
            }
            if (!py::isinstance<ImageBuffer>(result)) {
                last_error_ = "process() must return visiong.ImageBuffer or None";
                return std::nullopt;
            }

            ImageBuffer frame = result.cast<ImageBuffer>();
            if (!frame.is_valid()) {
                return std::nullopt;
            }

            return frame;
        } catch (const std::exception& e) {
            last_error_ = e.what();
            return std::nullopt;
        }
    }

    std::string GetCode() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return code_;
    }

    std::string GetLastError() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_;
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        py::gil_scoped_acquire gil;
        try {
            if (!cleanup_fn_.is_none()) {
                cleanup_fn_();
            }
        } catch (...) {
        }

        cleanup_fn_ = py::none();
        init_fn_ = py::none();
        process_fn_ = py::none();
        globals_ = py::dict();
    }

private:
    mutable std::mutex mutex_;
    py::object globals_;
    py::object process_fn_;
    py::object init_fn_;
    py::object cleanup_fn_;
    std::string code_;
    std::string last_error_;
};

}  // namespace

struct VisionGProducer::Impl {
    SerialStreamDispatcher dispatcher;
    std::unique_ptr<PythonRuntime> runtime;
};

void WarmupVisionGPythonRuntime() {
    try {
        EnsureEmbeddedPythonReady();
        LOG_INFO("VisionG Python runtime warmup finished");
    } catch (const std::exception& e) {
        LOG_ERROR("VisionG Python runtime warmup failed: {}", e.what());
    }
}

VisionGProducer::VisionGProducer(const ProducerConfig& config)
    : config_(config), type_name_("VisionG/Python"), impl_(std::make_unique<Impl>()) {
}

VisionGProducer::~VisionGProducer() {
    Deinit();
}

int VisionGProducer::Init() {
    if (initialized_.load()) {
        return 0;
    }

    LOG_INFO("Initializing VisionG producer (python-managed frame pipeline)");

    impl_->runtime = std::make_unique<PythonRuntime>();

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (current_code_.empty()) {
            current_code_ = kDefaultVisionGScript;
        }
        const std::string err = impl_->runtime->LoadCode(current_code_);
        if (!err.empty()) {
            last_error_ = err;
            LOG_ERROR("Failed to load initial Python code: {}", err);
            impl_->runtime.reset();
            return -1;
        }
        last_error_.clear();
    }

    initialized_.store(true);
    LOG_INFO("VisionG producer initialized");
    return 0;
}

int VisionGProducer::Deinit() {
    if (!initialized_.load()) {
        return 0;
    }

    Stop();

    if (impl_->runtime) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        impl_->runtime->Shutdown();
        impl_->runtime.reset();
    }

    VencManager::getInstance().releaseVencIfUnused();

    initialized_.store(false);
    LOG_INFO("VisionG producer deinitialized");
    return 0;
}

bool VisionGProducer::Start() {
    if (!initialized_.load()) {
        LOG_ERROR("VisionG producer not initialized");
        return false;
    }
    if (running_.load()) {
        return true;
    }

    running_.store(true);
    frame_thread_ = std::thread(&VisionGProducer::FrameLoop, this);
    LOG_INFO("VisionG producer started");
    return true;
}

void VisionGProducer::Stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);
    if (frame_thread_.joinable()) {
        frame_thread_.join();
    }

    LOG_INFO("VisionG producer stopped");
}

void VisionGProducer::RegisterStreamConsumer(const std::string& name, StreamCallback callback,
                                             StreamConsumerType type, int queue_size) {
    (void)queue_size;
    impl_->dispatcher.RegisterConsumer(name, std::move(callback), type);
}

void VisionGProducer::ClearStreamConsumers() {
    impl_->dispatcher.ClearConsumers();
}

int VisionGProducer::SetResolution(Resolution preset) {
    config_.resolution = preset;
    return 0;
}

int VisionGProducer::SetFrameRate(int fps) {
    config_.framerate = std::max(1, std::min(fps, 30));
    return 0;
}

std::string VisionGProducer::GetCurrentCode() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_code_;
}

std::string VisionGProducer::GetLastError() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_error_;
}

std::string VisionGProducer::UpdateCode(const std::string& code) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (!impl_->runtime) {
        last_error_ = "Python runtime not initialized";
        return last_error_;
    }

    const std::string err = impl_->runtime->LoadCode(code);

    if (err.empty()) {
        current_code_ = code;
        last_error_.clear();
    } else {
        last_error_ = err;
    }
    return err;
}

void VisionGProducer::FrameLoop() {
    auto& venc = VencManager::getInstance();
    VencManager::ScopedUser user(venc);

    while (running_.load()) {
        if (!impl_->runtime) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        auto frame_opt = impl_->runtime->ProcessFrame();
        if (!frame_opt.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        VencEncodedPacket packet;
        const bool ok = venc.encodeToVideo(
            *frame_opt,
            VencCodec::H264,
            75,
            packet,
            config_.framerate,
            VencRcMode::CBR);
        if (!ok) {
            LOG_WARN("VisionG encode failed");
            continue;
        }

        auto stream = ConvertPacketToEncodedStream(packet);
        if (!stream) {
            continue;
        }

        frame_count_++;
        encode_count_++;
        impl_->dispatcher.DispatchFrame(stream);
    }

    LOG_INFO("VisionG frame loop exited, frames={}, encoded={}", frame_count_.load(), encode_count_.load());
}

std::unique_ptr<IMediaProducer> CreateVisionGProducer(const ProducerConfig& config) {
    return std::make_unique<VisionGProducer>(config);
}

}  // namespace media
