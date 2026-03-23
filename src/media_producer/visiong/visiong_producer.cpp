/**
 * @file visiong_producer.cpp
 * @brief VisionG Python 驱动模式生产者实现
 *
 * 架构：Python 脚本通过 visiong.Camera 自主驱动帧循环，
 * 调用 aipc.submit_frame(frame) 将处理后的帧推送给 C++ 进行 VENC 编码和流媒体分发。
 * C++ 仅负责 Python 解释器生命周期、工程加载/热更新、编码和分发。
 *
 * aipc 模块（pybind11 embedded）：
 *   aipc.submit_frame(frame)  将 ImageBuffer 推入 C++ 编码流水线
 *   aipc.is_running()         返回 bool，供 Python 帧循环条件判断
 */

#define LOG_TAG "VisionGProd"

#include "visiong_producer.h"

#include "common/asio_context.h"
#include "common/logger.h"
#include "common/media_buffer.h"

#include "visiong/core/ImageBuffer.h"
#include "visiong/modules/VencManager.h"

#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"

#include <atomic>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include <pybind11/embed.h>

namespace py = pybind11;

namespace media {

    namespace {

        // ============================================================================
        // SerialStreamDispatcher
        // ============================================================================

        class SerialStreamDispatcher {
        public:
            struct ConsumerInfo {
                std::string name;
                StreamCallback callback;
                StreamConsumerType type;
            };

            void RegisterConsumer(const std::string &name, StreamCallback callback, StreamConsumerType type) {
                consumers_.push_back({name, std::move(callback), type});
                LOG_INFO("Registered stream consumer: {}", name);
            }

            void ClearConsumers() { consumers_.clear(); }

            void DispatchFrame(const EncodedStreamPtr &stream) {
                for (auto &c: consumers_) {
                    if (!c.callback)
                        continue;
                    if (c.type == StreamConsumerType::AsyncIO) {
                        PostToIo([cb = c.callback, stream]() { cb(stream); });
                    } else {
                        c.callback(stream);
                    }
                }
            }

        private:
            std::vector<ConsumerInfo> consumers_;
        };

        // ============================================================================
        // Encoded-stream conversion helpers
        // ============================================================================

        struct VisionGPacketOpaque {
            std::vector<unsigned char> payload;
        };

        static RK_S32 ReleaseVisionGPacketOpaque(void *opaque) {
            delete static_cast<VisionGPacketOpaque *>(opaque);
            return RK_SUCCESS;
        }

        static RK_U64 GetNowUs() {
            struct timespec ts = {0, 0};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            return static_cast<RK_U64>(ts.tv_sec) * 1000000ULL + static_cast<RK_U64>(ts.tv_nsec) / 1000ULL;
        }

        static EncodedStreamPtr ConvertPacketToEncodedStream(const VencEncodedPacket &packet) {
            if (packet.data.empty())
                return nullptr;

            auto *stream = new VENC_STREAM_S();
            auto *pack = new VENC_PACK_S();
            memset(stream, 0, sizeof(VENC_STREAM_S));
            memset(pack, 0, sizeof(VENC_PACK_S));
            stream->pstPack = pack;

            auto *opaque = new VisionGPacketOpaque();
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
                LOG_ERROR("RK_MPI_SYS_CreateMB failed (size={})", packet.data.size());
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

            return EncodedStreamPtr(stream, [](VENC_STREAM_S *p) {
                if (!p)
                    return;
                if (p->pstPack && p->pstPack->pMbBlk != MB_INVALID_HANDLE) {
                    RK_MPI_MB_ReleaseMB(p->pstPack->pMbBlk);
                    p->pstPack->pMbBlk = MB_INVALID_HANDLE;
                }
                delete p->pstPack;
                delete p;
            });
        }

        // ============================================================================
        // Dynamic quality helper
        // ============================================================================

        /**
         * @brief 按帧像素数动态选编码质量档位
         *
         *   >= 1080p → 80   高码率，保画质
         *   >=  720p → 75   中等码率
         *   >=  480p → 70   低码率，节省带宽
         *    < 480p  → 65   更小分辨率
         */
        static int ComputeQualityFromFrame(const ImageBuffer &frame) {
            const int pixels = frame.width * frame.height;
            if (pixels >= 1920 * 1080)
                return 80;
            if (pixels >= 1280 * 720)
                return 75;
            if (pixels >= 640 * 480)
                return 70;
            return 65;
        }

        // ============================================================================
        // Default Python script (Python-driven passthrough)
        // ============================================================================

        static const char *kDefaultVisionGScript = R"PY(
# Default VisionG project: camera passthrough
#
# Python 驱动架构：Python 自主创建摄像头、驱动帧循环，
# 通过 aipc.submit_frame() 将帧送 C++ 进行 H.264 编码和流媒体分发。
#
# 契约：
#   init()      可选，初始化摄像头和模型等资源
#   run()       必须，驱动帧循环直至 aipc.is_running() 返回 False
#   cleanup()   可选，释放资源

import visiong
import aipc

_cam = None


def init():
    global _cam
    _cam = visiong.Camera(640, 360, format='rgb')
    _cam.skip(8)


def run():
    while aipc.is_running():
        frame = _cam.snapshot()
        if not frame.is_valid():
            continue
        aipc.submit_frame(frame)


def cleanup():
    global _cam
    if _cam:
        _cam.release()
        _cam = None
)PY";

        // ============================================================================
        // EnsureEmbeddedPythonReady
        // ============================================================================

        void EnsureEmbeddedPythonReady() {
            static std::once_flag init_once;

            // 只会执行一次lambda
            std::call_once(init_once, []() {
                LOG_INFO("[PythonInit] initialize_interpreter begin");
                py::initialize_interpreter(false, 0, nullptr, false);
                LOG_INFO("[PythonInit] initialize_interpreter done");
                {
                    py::gil_scoped_acquire gil;
                    py::module_ sys = py::module_::import("sys");
                    auto path = sys.attr("path").cast<py::list>();
                    path.append("../python");
                }
                LOG_INFO("[PythonInit] Embedded Python ready, sys.path updated");
            });
        }

        // ============================================================================
        // aipc pybind11 嵌入模块全局状态
        // ============================================================================
        static std::mutex g_aipc_mutex;
        static std::function<void(const ImageBuffer &)> g_submit_frame_cb;
        static std::atomic<bool> g_is_running{false};

        PYBIND11_EMBEDDED_MODULE(aipc, m) {
            m.doc() = "aipc: C++ 编码接口，供 VisionG Python 脚本调用";
            m.def(
                    "submit_frame",
                    [](const ImageBuffer &frame) {
                        std::function<void(const ImageBuffer &)> cb;
                        {
                            std::lock_guard<std::mutex> lock(g_aipc_mutex);
                            cb = g_submit_frame_cb;
                        }
                        if (cb) {
                            cb(frame);
                        }
                    },
                    py::arg("frame"), "将处理后的 ImageBuffer 推入 C++ VENC 编码队列。");
            m.def(
                    "is_running", []() -> bool { return g_is_running.load(); },
                    "返回 True 表示生产者正在运行，False 表示应退出帧循环。");
        }

        // ============================================================================
        // PythonRuntime
        // ============================================================================

        /**
         * @class PythonRuntime
         * @brief 管理单个 Python 脚本的生命周期
         *
         * LoadCode 操作序：
         *   旧 cleanup()（尽力）→ 清空旧 state → exec 新代码 →
         *   验签名（run 必须存在）→ 调新 init()（失败则回滚）→
         *   全部通过后才提交到成员变量。
         *
         * 错误分三类：[exec error] / [signature error] / [init error]
         */
        class PythonRuntime {
        public:
            PythonRuntime() {
                LOG_INFO("[PythonRuntime] ctor begin");

                // 这个函数调用了 std::call_once，确保 Python 解释器在任何 PythonRuntime 实例创建前就已初始化，因此只会执行一次。
                EnsureEmbeddedPythonReady();
                py::gil_scoped_acquire gil;
                globals_ = py::dict();
                run_fn_ = py::none();
                init_fn_ = py::none();
                cleanup_fn_ = py::none();
                LOG_INFO("[PythonRuntime] ctor done");
            }

            ~PythonRuntime() { Shutdown(); }

            // 禁止拷贝
            PythonRuntime(const PythonRuntime &) = delete;
            PythonRuntime &operator=(const PythonRuntime &) = delete;

            // ------------------------------------------------------------------
            // LoadCode
            // ------------------------------------------------------------------

            /**
             * @brief 加载/更新 Python 代码
             * @return 空字符串表示成功；非空字符串为带分类前缀的错误描述
             */
            std::string LoadCode(const std::string &code) {
                std::lock_guard<std::mutex> lock(mutex_);
                py::gil_scoped_acquire gil;

                LOG_INFO("[LoadCode] begin");

                // ── Step 1: 调旧 cleanup()（尽力而为，失败只 WARN）────────────
                if (!cleanup_fn_.is_none()) {
                    LOG_INFO("[LoadCode] calling old cleanup()");
                    try {
                        cleanup_fn_();
                        LOG_INFO("[LoadCode] old cleanup() done");
                    } catch (const std::exception &e) {
                        LOG_WARN("[LoadCode] old cleanup() threw (continuing): {}", e.what());
                    } catch (...) {
                        LOG_WARN("[LoadCode] old cleanup() threw unknown (continuing)");
                    }
                }

                // 无论旧 cleanup 成败，立即清空旧 state，避免残留
                cleanup_fn_ = py::none();
                init_fn_ = py::none();
                run_fn_ = py::none();
                globals_ = py::dict();

                // ── Step 2: exec 新代码 ──────────────────────────────────────
                LOG_INFO("[LoadCode] exec begin");
                py::dict new_globals;
                try {
                    new_globals["__builtins__"] = py::module_::import("builtins");
                    py::exec(code, new_globals);
                } catch (const std::exception &e) {
                    last_error_ = std::string("[exec error] ") + e.what();
                    LOG_ERROR("[LoadCode] {}", last_error_);
                    return last_error_;
                }
                LOG_INFO("[LoadCode] exec done");

                // ── Step 3: 验签名（run 必须存在） ───────────────────────────
                py::object new_run = new_globals.contains("run") ? new_globals["run"] : py::none();
                if (new_run.is_none() || !py::isinstance<py::function>(new_run)) {
                    last_error_ = "[signature error] Python code must define callable: run()";
                    LOG_ERROR("[LoadCode] {}", last_error_);
                    return last_error_;
                }

                py::object new_init = new_globals.contains("init") ? new_globals["init"] : py::none();
                py::object new_cleanup = new_globals.contains("cleanup") ? new_globals["cleanup"] : py::none();

                // ── Step 4: 调新 init()（成功后才提交，失败则回滚）────────────
                if (!new_init.is_none()) {
                    LOG_INFO("[LoadCode] calling new init()");
                    try {
                        new_init();
                        LOG_INFO("[LoadCode] new init() done");
                    } catch (const std::exception &e) {
                        // init 失败：尝试回调 new_cleanup 清理本次已分配的资源
                        if (!new_cleanup.is_none()) {
                            try {
                                new_cleanup();
                            } catch (...) {
                            }
                        }
                        last_error_ = std::string("[init error] ") + e.what();
                        LOG_ERROR("[LoadCode] init() failed, rolled back: {}", last_error_);
                        return last_error_;
                    }
                }

                // ── Step 5: 提交新 state ─────────────────────────────────────
                globals_ = std::move(new_globals);
                run_fn_ = std::move(new_run);
                init_fn_ = std::move(new_init);
                cleanup_fn_ = std::move(new_cleanup);
                code_ = code;
                last_error_.clear();

                LOG_INFO("[LoadCode] committed successfully");
                return "";
            }

            // ------------------------------------------------------------------
            // CallRun
            // ------------------------------------------------------------------

            void CallRun() {
                py::gil_scoped_acquire gil;
                py::object fn;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    fn = run_fn_;
                }
                if (fn.is_none()) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    last_error_ = "run() not ready";
                    LOG_ERROR("[CallRun] {}", last_error_);
                    return;
                }
                LOG_INFO("[CallRun] calling Python run()");
                try {
                    fn();
                    LOG_INFO("[CallRun] Python run() returned normally");
                } catch (const std::exception &e) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    last_error_ = e.what();
                    LOG_WARN("[CallRun] Python run() threw: {}", last_error_);
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
                    // Shutdown 期间静默忽略
                }
                cleanup_fn_ = py::none();
                init_fn_ = py::none();
                run_fn_ = py::none();
                globals_ = py::dict();
            }

        private:
            mutable std::mutex mutex_;

            py::object globals_;
            py::object run_fn_;
            py::object init_fn_;
            py::object cleanup_fn_;

            std::string code_;
            std::string last_error_;
        };

    } // anonymous namespace

    // ============================================================================
    // VisionGProducer::Impl
    // ============================================================================

    struct VisionGProducer::Impl {
        SerialStreamDispatcher dispatcher;
        // runtime は shared_ptr なので UpdateCode と RunPythonScript が安全に共存できる
        std::shared_ptr<PythonRuntime> runtime;
    };

    // ============================================================================
    // WarmupVisionGPythonRuntime
    // ============================================================================

    void WarmupVisionGPythonRuntime() {
        try {
            EnsureEmbeddedPythonReady();
            LOG_INFO("VisionG Python runtime warmup finished");
        } catch (const std::exception &e) {
            LOG_ERROR("VisionG Python runtime warmup failed: {}", e.what());
        }
    }

    // ============================================================================
    // VisionGProducer 生命周期
    // ============================================================================

    VisionGProducer::VisionGProducer(const ProducerConfig &config) :
        config_(config), type_name_("VisionG/Python"), impl_(std::make_unique<Impl>()) {}

    VisionGProducer::~VisionGProducer() { Deinit(); }

    int VisionGProducer::Init() {
        if (initialized_.load()) {
            return 0;
        }

        LOG_INFO("Initializing VisionG producer (Python-driven: Python owns camera + frame loop)");

        // ── 创建 Python 运行时 ───────────────────────────────────────────────────
        impl_->runtime = std::make_shared<PythonRuntime>();

        // ── 注册 aipc.submit_frame 回调（编码并分发） ───────────────────────────
        {
            std::lock_guard<std::mutex> lock(g_aipc_mutex);
            g_submit_frame_cb = [this](const ImageBuffer &frame) { EncodeAndDispatch(frame); };
        }

        // ── 加载初始 Python 脚本 ─────────────────────────────────────────────────
        std::string code_to_load;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            code_to_load = current_code_.empty() ? std::string(kDefaultVisionGScript) : current_code_;
        }

        const std::string err = impl_->runtime->LoadCode(code_to_load);
        if (!err.empty()) {
            LOG_ERROR("[Init] Failed to load initial Python script: {}", err);
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                last_error_ = err;
            }
            impl_->runtime.reset();
            {
                std::lock_guard<std::mutex> lock(g_aipc_mutex);
                g_submit_frame_cb = nullptr;
            }
            return -1;
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (current_code_.empty()) {
                current_code_ = kDefaultVisionGScript;
            }
            last_error_.clear();
        }

        initialized_.store(true);
        LOG_INFO("VisionG producer initialized (Python will drive camera + frame loop via aipc)");
        return 0;
    }

    int VisionGProducer::Deinit() {
        if (!initialized_.load()) {
            return 0;
        }

        Stop();

        // 清空 aipc 回调，防止 Python 脚本退出后残留调用
        {
            std::lock_guard<std::mutex> lock(g_aipc_mutex);
            g_submit_frame_cb = nullptr;
        }
        g_is_running.store(false);

        if (impl_->runtime) {
            impl_->runtime->Shutdown();
            impl_->runtime.reset();
        }

        initialized_.store(false);
        LOG_INFO("VisionG producer deinitialized");
        return 0;
    }

    bool VisionGProducer::Start() {
        if (!initialized_.load()) {
            LOG_ERROR("[VisionG] Not initialized");
            return false;
        }
        if (running_.load()) {
            LOG_WARN("[VisionG] Already running");
            return true;
        }

        g_is_running.store(true);
        running_.store(true);
        script_thread_ = std::thread(&VisionGProducer::RunPythonScript, this);
        LOG_INFO("[VisionG] started: Python script thread launched");
        return true;
    }

    void VisionGProducer::Stop() {
        if (!running_.load()) {
            return;
        }

        LOG_INFO("[VisionG] Stop: signaling Python run() to exit via aipc.is_running() = false");
        g_is_running.store(false);
        running_.store(false);

        if (script_thread_.joinable()) {
            script_thread_.join();
        }

        LOG_INFO("[VisionG] stopped (frames={}, encoded={})", frame_count_.load(), encode_count_.load());
    }

    // ============================================================================
    // 流消费者管理
    // ============================================================================

    void VisionGProducer::RegisterStreamConsumer(const std::string &name, StreamCallback callback,
                                                 StreamConsumerType type, int queue_size) {
        (void) queue_size;
        impl_->dispatcher.RegisterConsumer(name, std::move(callback), type);
    }

    void VisionGProducer::ClearStreamConsumers() { impl_->dispatcher.ClearConsumers(); }

    // ============================================================================
    // Python 代码管理
    // ============================================================================

    std::string VisionGProducer::GetCurrentCode() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return current_code_;
    }

    std::string VisionGProducer::GetLastError() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return last_error_;
    }

    std::string VisionGProducer::UpdateCode(const std::string &code) {
        // 1. 停止当前运行的脚本（向 aipc.is_running() 发 false，等待 run() 返回）
        bool was_running = running_.load();
        if (was_running) {
            LOG_INFO("[UpdateCode] stopping running script before reload");
            g_is_running.store(false);
            running_.store(false);
            if (script_thread_.joinable()) {
                script_thread_.join();
            }
        }

        // 2. 获取运行时并加载新代码
        std::shared_ptr<PythonRuntime> runtime;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            runtime = impl_->runtime;
        }

        if (!runtime) {
            return "[error] Runtime not initialized";
        }

        const std::string err = runtime->LoadCode(code);

        if (err.empty()) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            current_code_ = code;
            last_error_.clear();
        } else {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_error_ = err;
        }

        // 3. 若之前在运行且加载成功，重新启动脚本线程
        if (was_running && err.empty()) {
            LOG_INFO("[UpdateCode] restarting script thread with new code");
            g_is_running.store(true);
            running_.store(true);
            script_thread_ = std::thread(&VisionGProducer::RunPythonScript, this);
        }

        return err;
    }

    // ============================================================================
    // EncodeAndDispatch（由 aipc.submit_frame 回调调用）
    // ============================================================================

    void VisionGProducer::EncodeAndDispatch(const ImageBuffer &frame) {
        auto &venc = VencManager::getInstance();

        const int quality = ComputeQualityFromFrame(frame);
        VencEncodedPacket packet;
        const bool encoded =
                venc.encodeToVideo(frame, VencCodec::H264, quality, packet, config_.framerate, VencRcMode::CBR);

        if (!encoded) {
            LOG_WARN("[VisionG] encode failed (quality={}, size={}x{})", quality, frame.width, frame.height);
            return;
        }

        auto stream = ConvertPacketToEncodedStream(packet);
        if (!stream) {
            return;
        }

        ++frame_count_;
        ++encode_count_;
        impl_->dispatcher.DispatchFrame(stream);

        const uint64_t fc = frame_count_.load();
        if (fc > 0 && fc % 300 == 0) {
            LOG_INFO("[VisionG] stats: frames={}, encoded={}", fc, encode_count_.load());
        }
    }

    // ============================================================================
    // RunPythonScript（后台线程入口）
    // ============================================================================

    void VisionGProducer::RunPythonScript() {
        auto &venc = VencManager::getInstance();
        VencManager::ScopedUser user(venc);

        LOG_INFO("[RunPythonScript] started");

        std::shared_ptr<PythonRuntime> runtime;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            runtime = impl_->runtime;
        }

        if (!runtime) {
            LOG_ERROR("[RunPythonScript] No runtime available");
            return;
        }

        // 调用 Python 脚本的 run() 函数（阻塞直到 Python run() 返回）
        // Python 脚本通过 aipc.is_running() 判断是否继续循环，
        // 通过 aipc.submit_frame(frame) 将帧推入 C++ 编码。
        runtime->CallRun();

        LOG_INFO("[RunPythonScript] Python run() exited (frames={}, encoded={})", frame_count_.load(),
                 encode_count_.load());
    }

    // ============================================================================
    // 工厂函数
    // ============================================================================

    std::unique_ptr<IMediaProducer> CreateVisionGProducer(const ProducerConfig &config) {
        return std::make_unique<VisionGProducer>(config);
    }

} // namespace media
