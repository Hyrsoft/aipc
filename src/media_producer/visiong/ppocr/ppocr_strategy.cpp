/**
 * @file ppocr_strategy.cpp
 * @brief PPOCR 文字识别 - 推理 + OSD 绘制（检测框 + 识别文字）
 */

#include "ppocr_strategy.h"
#include "common/logger.h"

#include "visiong/core/ImageBuffer.h"
#include "visiong/npu/PPOCR.h"

namespace media {

PpocrStrategy::PpocrStrategy(const std::string& det_model_path,
                             const std::string& rec_model_path,
                             const std::string& dict_path)
    : det_model_path_(det_model_path),
      rec_model_path_(rec_model_path),
      dict_path_(dict_path) {}

PpocrStrategy::~PpocrStrategy() = default;

bool PpocrStrategy::Init() {
    ppocr_ = std::make_unique<PPOCR>(det_model_path_, rec_model_path_, dict_path_);
    if (!ppocr_->is_initialized()) {
        LOG_ERROR("PPOCR init failed: det={}, rec={}", det_model_path_, rec_model_path_);
        ppocr_.reset();
        return false;
    }
    return true;
}

void PpocrStrategy::Deinit() {
    ppocr_.reset();
}

ImageBuffer PpocrStrategy::ProcessFrame(const ImageBuffer& frame, NPU* /*npu*/) {
    auto results = ppocr_->infer(frame);

    ImageBuffer draw_frame = frame.get_bgr_version().copy();
    for (const auto& r : results) {
        auto [rx, ry, rw, rh] = r.rect;
        draw_frame.draw_rectangle(rx, ry, rw, rh, {0, 255, 0}, 2, false);

        // 在框上方绘制识别到的文字
        if (!r.text.empty()) {
            draw_frame.draw_string(rx, ry - 8, r.text, {255, 255, 0}, 1.0, 2);
        }
    }
    return draw_frame;
}

} // namespace media
