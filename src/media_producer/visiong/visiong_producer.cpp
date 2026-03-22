/**
 * @file visiong_producer.cpp
 * @brief VisionG Python 模式生产者 — 全量重写（Phase A + B + C）
 *
 * Phase A（稳定性止血）
 *   - LoadCode 修复：先清旧态 → exec → 验签名 → 调 init() → 成功后才提交
 *     任何阶段失败均携带分类前缀 [exec error] / [signature error] / [init error]
 *   - 旧 cleanup() 失败只记 WARN，不阻止新代码加载
 *   - impl_->runtime 改为 shared_ptr，FrameLoop 每次迭代持有局部引用
 *     → 彻底消除检查/使用分离窗口
 *   - UpdateCode 仅在极短区间持有 state_mutex_（取副本和写结果），
 *     LoadCode 本身由 PythonRuntime 内部 mutex + GIL 自行保护
 *
 * Phase B（架构对齐：C++ 主控采集）
 *   - Impl 新增 std::unique_ptr<Camera>，由 Init() 创建、Deinit() 释放
 *   - FrameLoop 变为：camera->snapshot() → runtime->ProcessFrame(frame) → encode
 *   - Python 契约从 process() 改为 process(frame)，脚本不再自建采集循环
 *   - kDefaultVisionGScript 更新为最简透传示例
 *
 * Phase C（吞吐与质量优化）
 *   - ComputeQualityFromFrame()：按帧像素数动态选质量档位，替代固定 75
 *   - 移除 process() 返回 None 时的 5ms sleep（camera 天然限速）
 *   - 每 300 帧记录一次统计日志
 */

#define LOG_TAG "VisionGProd"

#include "visiong_producer.h"

#include "common/asio_context.h"
#include "common/logger.h"
#include "common/media_buffer.h"

#include "visiong/core/Camera.h"
#include "visiong/core/ImageBuffer.h"
#include "visiong/modules/VencManager.h"

#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <optional>
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
        // Phase C: dynamic quality
        // ============================================================================

        /**
 * @brief 按
帧像素数动态选编码质量档位
 *
 * 替代原先固定 quality=75 的策略：
 *   ≥ 1080p → 80   高码率，保画质
 *   ≥  720p → 75   中等码率
 *   ≥  480p → 70   低码率，节省带宽
 *    < 480p → 65   更小分辨率
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
        // Default Python script (Phase B contract: process(frame) -> frame | None)
        // ============================================================================

        static const char *kDefaultVisionGScript = R"PY(
# Default VisionG project: passthrough
#
# C++ 驱动帧循环：Camera.snapshot() -> process(frame) -> encode -> distribute
# 本脚本直接透传输入帧；
替换 process() 内容即可实现自定义推理与绘制。
#
# 契约：
#   init()        可选，模块加载时调用一次（初始化模型等资源）
#   process(frame) 必须，每帧调用；返回 ImageBuffer 或 None（跳过该帧）
#   cleanup()     可选，模块卸载时调用一次（释放资源）

def init():
    pass


def process(frame):
    # Passthrough: return the frame as-is.
    # Replace with your own inference / drawing logic.
    return frame


def cleanup():
    pass
)PY";

        // ============================================================================
        // EnsureEmbeddedPythonReady
        // ============================================================================

        void EnsureEmbeddedPythonReady() {
            static std::once_flag init_once;
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
        // PythonRuntime — Phase A + B
        // ============================================================================

        /**
         * @class PythonRuntime
         * @brief 管理单个 Python 脚本的生命周期与帧处理
         *
         * Phase A 修复要点
         * ────────────────
         * 1. LoadCode 操作序：
         *      旧 cleanup()（尽力）→ 清空旧 state → exec 新代码 →
         *      验签名 → 调新 init()（失败则回滚+回调新 cleanup） →
         *      全部通过后才提交到成员变量。
         * 2. 错误分三类：[exec error] / [signature error] / [init error]
         * 3. 旧 cleanup() 抛异常仅记 WARN，不阻断新代码加载。
         *
         * Phase B 修复要点
         * ────────────────
         * ProcessFrame(const ImageBuffer&)：接收 C++ 采集的帧，
         * 通过 pybind11 传给 Python 的 process(frame)，返回处理后的帧。
         */
        class PythonRuntime {
        public:
            PythonRuntime() {
                LOG_INFO("[PythonRuntime] ctor begin");
                EnsureEmbeddedPythonReady();
                py::gil_scoped_acquire gil;
                globals_ = py::dict();
                process_fn_ = py::none();
                init_fn_ = py::none();
                cleanup_fn_ = py::none();
                LOG_INFO("[PythonRuntime] ctor done");
            }

            ~PythonRuntime() { Shutdown(); }

            // 禁止拷贝
            PythonRuntime(const PythonRuntime &) = delete;
            PythonRuntime &operator=(const PythonRuntime &) = delete;

            // ------------------------------------------------------------------
            // LoadCode（Phase A 重写）
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
                process_fn_ = py::none();
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

                // ── Step 3: 验签名 ───────────────────────────────────────────
                py::object new_process = new_globals.contains("process") ? new_globals["process"] : py::none();
                if (new_process.is_none() || !py::isinstance<py::function>(new_process)) {
                    last_error_ = "[signature error] Python code must define callable: process(frame)";
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
                process_fn_ = std::move(new_process);
                init_fn_ = std::move(new_init);
                cleanup_fn_ = std::move(new_cleanup);
                code_ = code;
                last_error_.clear();

                LOG_INFO("[LoadCode] committed successfully");
                return "";
            }

            // ------------------------------------------------------------------
            // ProcessFrame（Phase B：接受 C++ 提供的输入帧）
            // ------------------------------------------------------------------

            /**
             * @brief 调用 Python process(frame)
             *
             * @param input_frame  由 Camera::snapshot() 获取的输入帧
             * @return 处理后的 ImageBuffer；Python 返回 None 或出错则 std::nullopt
             */
            std::optional<ImageBuffer> ProcessFrame(const ImageBuffer &input_frame) {
                std::lock_guard<std::mutex> lock(mutex_);
                py::gil_scoped_acquire gil;

                try {
                    if (process_fn_.is_none()) {
                        last_error_ = "process() not ready";
                        return std::nullopt;
                    }

                    // pybind11 自动将 C++ ImageBuffer 包装为 Python 对象传入
                    py::object result = process_fn_(input_frame);

                    if (result.is_none()) {
                        return std::nullopt;
                    }

                    if (!py::isinstance<ImageBuffer>(result)) {
                        last_error_ = "process(frame) must return visiong.ImageBuffer or None";
                        LOG_WARN("[ProcessFrame] {}", last_error_);
                        return std::nullopt;
                    }

                    ImageBuffer out = result.cast<ImageBuffer>();
                    if (!out.is_valid()) {
                        return std::nullopt;
                    }

                    return out;

                } catch (const std::exception &e) {
                    last_error_ = e.what();
                    LOG_WARN("[ProcessFrame] exception: {}", last_error_);
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
                    // Shutdown 期间静默忽略
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

    } // anonymous namespace

    // ============================================================================
    // VisionGProducer::Impl
    // Phase A: runtime is shared_ptr so FrameLoop holds a safe local reference.
    // Phase B: camera is owned by C++; Python scripts no longer create cameras.
    // ============================================================================

    struct VisionGProducer::Impl {
        SerialStreamDispatcher dispatcher;

        // Phase A: shared_ptr allows FrameLoop and UpdateCode to coexist safely.
        // FrameLoop captures a local copy each iteration; even if Deinit resets
        // the pointer concurrently, the PythonRuntime object is not destroyed
        // until the last reference is released.
        std::shared_ptr<PythonRuntime> runtime;

        // Phase B: C++ owns the camera; lifecycle managed by Init/Deinit.
        // Python scripts receive each frame via process(frame) instead of
        // creating their own capture loop.
        std::unique_ptr<Camera> camera;
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

        const int cam_w = config_.ai_width;
        const int cam_h = config_.ai_height;

        LOG_INFO("Initializing VisionG producer "
                 "(C++ camera {}x{} rgb + Python processing pipeline)",
                 cam_w, cam_h);

        // ── 创建 Python 运行时 ───────────────────────────────────────────
        impl_->runtime = std::make_shared<PythonRuntime>();

        // ── 创建并初始化摄像头（Phase B）────────────────────────────────
        auto camera = std::make_unique<Camera>(cam_w, cam_h, "rgb");
        if (!camera->is_initialized()) {
            LOG_ERROR("Camera init failed ({}x{} rgb). "
                      "Check ISP/VI hardware availability.",
                      cam_w, cam_h);
            impl_->runtime.reset();
            return -1;
        }
        // 跳过初始帧，等待 ISP AE/AWB 稳定
        camera->skip(8);
        impl_->camera = std::move(camera);
        LOG_INFO("[Init] camera ready: {}x{} rgb", cam_w, cam_h);

        // ── 加载初始 Python 代码（不持 state_mutex_ 期间调 LoadCode）────
        std::string code_to_load;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            code_to_load = current_code_.empty() ? std::string(kDefaultVisionGScript) : current_code_;
        }

        const std::string err = impl_->runtime->LoadCode(code_to_load);
        if (!err.empty()) {
            LOG_ERROR("[Init] Failed to load initial Python code: {}", err);
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                last_error_ = err;
            }
            impl_->camera.reset();
            impl_->runtime.reset();
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
        LOG_INFO("VisionG producer initialized");
        return 0;
    }

    int VisionGProducer::Deinit() {
        if (!initialized_.load()) {
            return 0;
        }

        // Stop() 内部 join frame_thread_，之后 FrameLoop 一定已退出
        Stop();

        {
            std::lock_guard<std::mutex> lock(state_mutex_);

            // 先 Shutdown Python（执行旧 cleanup），再 reset
            if (impl_->runtime) {
                impl_->runtime->Shutdown();
                impl_->runtime.reset();
            }

            // 释放摄像头
            if (impl_->camera) {
                impl_->camera->release();
                impl_->camera.reset();
            }
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
    // 配置接口
    // ============================================================================

    int VisionGProducer::SetResolution(Resolution preset) {
        config_.resolution = preset;
        return 0;
    }

    int VisionGProducer::SetFrameRate(int fps) {
        config_.framerate = std::max(1, std::min(fps, 30));
        return 0;
    }

    // ============================================================================
    // Python 代码管理（Phase A 修复）
    // ============================================================================

    std::string VisionGProducer::GetCurrentCode() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return current_code_;
    }

    std::string VisionGProducer::GetLastError() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return last_error_;
    }

    /**
     * @brief 热更新 Python 代码（Phase A 修复）
     *
     * 修复原始实现的两个问题：
     *  1. 原实现在持有 state_mutex_ 期间调用 LoadCode()，LoadCode 内又持有
     *     runtime->mutex_ + GIL；而 FrameLoop 持有 runtime->mutex_ 时若触发
     *     Deinit/Stop 请求 state_mutex_，可形成长时阻塞窗口。
     *  2. 原实现 init() 调用在赋值之后，init() 失败时新 cleanup_fn_ 已写入，
     *     下次 LoadCode 会对未完全初始化的资源调 cleanup，可导致崩溃。
     *
     * 修复方式：仅在极短区间持有 state_mutex_（取 runtime 副本 / 写结果），
     * LoadCode 本身由 PythonRuntime 内部 mutex + GIL 自行序列化。
     */
    std::string VisionGProducer::UpdateCode(const std::string &code) {
        // ── Step 1: 在短临界区内获取 runtime 的 shared_ptr 副本 ──────────
        std::shared_ptr<PythonRuntime> runtime;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!impl_ || !impl_->runtime) {
                last_error_ = "Python runtime not initialized";
                return last_error_;
            }
            runtime = impl_->runtime;
        }

        // ── Step 2: 在 state_mutex_ 之外调用 LoadCode ────────────────────
        // LoadCode 内部持有 PythonRuntime::mutex_ + GIL，与 FrameLoop 的
        // ProcessFrame() 自然序列化，不需要 state_mutex_ 参与。
        const std::string err = runtime->LoadCode(code);

        // ── Step 3: 写回结果 ─────────────────────────────────────────────
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (err.empty()) {
                current_code_ = code;
                last_error_.clear();
            } else {
                last_error_ = err;
            }
        }

        return err;
    }

    // ============================================================================
    // FrameLoop（Phase A + B + C）
    // ============================================================================

    /**
     * @brief 帧处理主循环
     *
     * 流水线：camera->snapshot() → runtime->ProcessFrame(frame) → encode → dispatch
     *
     * Phase A：每次迭代持有 runtime 的 shared_ptr 局部副本，消除 TOCTOU。
     * Phase B：C++ Camera 提供原始帧，Python 只做推理与绘制。
     * Phase C：动态质量策略 + 每 300 帧记录统计日志。
     */
    void VisionGProducer::FrameLoop() {
        auto &venc = VencManager::getInstance();
        VencManager::ScopedUser user(venc);

        // Camera 在整个 FrameLoop 生命周期内不变（Deinit 必先 join 本线程）
        Camera *const camera = impl_->camera.get();

        LOG_INFO("[FrameLoop] started (camera: {}x{})", camera ? camera->target_width() : 0,
                 camera ? camera->target_height() : 0);

        while (running_.load()) {

            // ── Phase A: 每次迭代持有 runtime shared_ptr 副本 ────────────
            std::shared_ptr<PythonRuntime> runtime;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                runtime = impl_->runtime;
            }

            if (!runtime || !camera) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            // ── Phase B: C++ 采集帧 ──────────────────────────────────────
            // camera->snapshot() 在 ISP/VI 层阻塞直到新帧就绪，
            // 天然限速到摄像头帧率，不需要额外 sleep。
            ImageBuffer frame = camera->snapshot();
            if (!frame.is_valid()) {
                LOG_WARN("[FrameLoop] camera->snapshot() returned invalid frame");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            // ── Phase B: 传帧给 Python process(frame) ────────────────────
            auto out_opt = runtime->ProcessFrame(frame);
            if (!out_opt.has_value()) {
                // Python 返回 None：跳过本帧（无需 sleep，摄像头已限速）
                continue;
            }

            // ── Phase C: 动态质量 + 编码 ─────────────────────────────────
            const int quality = ComputeQualityFromFrame(*out_opt);

            VencEncodedPacket packet;
            const bool encoded =
                    venc.encodeToVideo(*out_opt, VencCodec::H264, quality, packet, config_.framerate, VencRcMode::CBR);

            if (!encoded) {
                LOG_WARN("[FrameLoop] encode failed (quality={}, size={}x{})", quality, out_opt->width,
                         out_opt->height);
                continue;
            }

            auto stream = ConvertPacketToEncodedStream(packet);
            if (!stream) {
                continue;
            }

            ++frame_count_;
            ++encode_count_;
            impl_->dispatcher.DispatchFrame(stream);

            // ── Phase C: 定期统计日志 ─────────────────────────────────────
            const uint64_t fc = frame_count_.load();
            if (fc > 0 && fc % 300 == 0) {
                LOG_INFO("[FrameLoop] stats: frames={}, encoded={}, "
                         "last_quality={}, out={}x{}",
                         fc, encode_count_.load(), quality, out_opt->width, out_opt->height);
            }
        }

        LOG_INFO("[FrameLoop] exited (frames={}, encoded={})", frame_count_.load(), encode_count_.load());
    }

    // ============================================================================
    // 工厂函数
    // ============================================================================

    std::unique_ptr<IMediaProducer> CreateVisionGProducer(const ProducerConfig &config) {
        return std::make_unique<VisionGProducer>(config);
    }

} // namespace media
