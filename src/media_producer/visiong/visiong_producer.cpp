/**
 * @file visiong_producer.cpp
 * @brief VisionG 库驱动的 AI 推理模式生产者实现
 *
 * 使用 VisionG Camera 取帧 + NPU 推理 + ImageBuffer OSD + VencManager 编码。
 * 输出侧通过适配层转换为 EncodedStreamPtr，保持现有分发接口兼容。
 */

#define LOG_TAG "VisionGProd"

#include "visiong_producer.h"
#include "common/asio_context.h"
#include "common/logger.h"
#include "common/media_buffer.h"

#include "visiong/core/Camera.h"
#include "visiong/core/ImageBuffer.h"
#include "visiong/modules/VencManager.h"
#include "visiong/npu/NPU.h"

#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <utility>
#include <vector>

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

    void ClearConsumers() { consumers_.clear(); }

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
    if (packet.is_keyframe) {
        pack->DataType.enH264EType = H264E_NALU_IDRSLICE;
    } else {
        pack->DataType.enH264EType = H264E_NALU_PSLICE;
    }

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

}  // namespace

struct VisionGProducer::Impl {
    SerialStreamDispatcher dispatcher;
};

VisionGProducer::VisionGProducer(const ProducerConfig& config,
                                 const std::string& model_path,
                                 const std::string& label_path)
    : config_(config),
      model_path_(model_path),
      label_path_(label_path),
      impl_(std::make_unique<Impl>()) {

    type_name_ = "VisionG";

    LOG_DEBUG("VisionGProducer created ({})", type_name_);
}

VisionGProducer::~VisionGProducer() {
    Deinit();
    LOG_DEBUG("VisionGProducer destroyed ({})", type_name_);
}

int VisionGProducer::Init() {
    if (initialized_.load()) {
        LOG_WARN("Already initialized");
        return 0;
    }

    auto res = config_.GetResolutionConfig();
    LOG_INFO("Initializing VisionG producer ({}): {}x{} @ {}fps",
             type_name_, res.width, res.height, res.framerate);

    camera_ = std::make_unique<Camera>(res.width, res.height, "yuv");
    if (!camera_ || !camera_->is_initialized()) {
        LOG_ERROR("VisionG Camera init failed");
        camera_.reset();
        return -1;
    }

    camera_->skip(5);

    npu_ = std::make_unique<NPU>(
        ModelType::YOLOV5,
        model_path_,
        label_path_,
        0.25f,
        0.45f);
    if (!npu_ || !npu_->is_initialized()) {
        LOG_ERROR("VisionG NPU init failed: model={}", model_path_);
        camera_->release();
        camera_.reset();
        npu_.reset();
        return -1;
    }

    initialized_.store(true);
    LOG_INFO("VisionG producer ({}) initialized successfully", type_name_);
    LOG_INFO("Pipeline: Camera::snapshot() -> NPU::inference() -> OSD -> VencManager");
    return 0;
}

int VisionGProducer::Deinit() {
    if (!initialized_.load()) {
        return 0;
    }

    Stop();

    npu_.reset();

    if (camera_) {
        camera_->release();
        camera_.reset();
    }

    VencManager::getInstance().releaseVencIfUnused();

    initialized_.store(false);
    LOG_INFO("VisionG producer ({}) deinitialized", type_name_);
    return 0;
}

bool VisionGProducer::Start() {
    if (!initialized_.load()) {
        LOG_ERROR("Not initialized");
        return false;
    }

    if (running_.load()) {
        LOG_WARN("Already running");
        return true;
    }

    running_.store(true);
    frame_thread_ = std::thread(&VisionGProducer::FrameLoop, this);
    LOG_INFO("VisionG producer ({}) started", type_name_);
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
    LOG_INFO("VisionG producer ({}) stopped", type_name_);
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
    if (running_.load()) {
        LOG_WARN("Cannot change resolution while running");
        return -1;
    }
    config_.resolution = preset;
    return 0;
}

int VisionGProducer::SetFrameRate(int fps) {
    if (running_.load()) {
        LOG_WARN("Cannot change framerate while running");
        return -1;
    }
    config_.framerate = std::max(1, std::min(fps, 30));
    return 0;
}

void VisionGProducer::FrameLoop() {
    LOG_INFO("VisionG frame loop started ({})", type_name_);

    auto& venc = VencManager::getInstance();
    VencManager::ScopedUser venc_user(venc);

    while (running_.load()) {
        ImageBuffer frame = camera_->snapshot();
        if (!frame.is_valid()) {
            LOG_WARN("Camera snapshot failed, retrying...");
            continue;
        }
        frame_count_++;

        auto detections = npu_->inference(frame);
        inference_count_++;

        ImageBuffer draw_frame = frame.get_bgr_version().copy();
        for (const auto& det : detections) {
            auto [x, y, w, h] = det.box;
            draw_frame.draw_rectangle(x, y, w, h, {0, 255, 0}, 3, false);

            char text[64];
            snprintf(text, sizeof(text), "%s %.1f%%", det.label.c_str(), det.score * 100.0f);
            draw_frame.draw_string(x, y - 8, text, {0, 255, 0}, 1.0, 2);
        }

        VencEncodedPacket packet;
        bool ok = venc.encodeToVideo(
            draw_frame,
            VencCodec::H264,
            75,
            packet,
            config_.framerate,
            VencRcMode::CBR);
        if (!ok) {
            LOG_WARN("VencManager encodeToVideo failed");
            continue;
        }

        auto stream = ConvertPacketToEncodedStream(packet);
        if (!stream) {
            LOG_WARN("Failed to convert VencEncodedPacket to EncodedStreamPtr");
            continue;
        }

        impl_->dispatcher.DispatchFrame(stream);
    }

    LOG_INFO("VisionG frame loop exited ({}), total frames: {}, inferences: {}",
             type_name_, frame_count_.load(), inference_count_.load());
}

std::unique_ptr<IMediaProducer> CreateVisionGYoloProducer(const ProducerConfig& config) {
    return std::make_unique<VisionGProducer>(
        config,
        "../model/yolov5.rknn",
        "../model/coco_80_labels_list.txt");
}

}  // namespace media
