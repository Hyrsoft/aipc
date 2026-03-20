/**
 * @file ppocr_strategy.h
 * @brief PPOCR 文字识别模型策略
 *
 * PPOCR 使用独立的 PPOCR 类而非通用 NPU 类，
 * 因此不覆写 CreateNPU()，在 Init/Deinit 中管理自身引擎。
 */

#pragma once

#include "../i_model_strategy.h"

#include <memory>
#include <string>

class PPOCR;

namespace media {

class PpocrStrategy : public IModelStrategy {
public:
    PpocrStrategy(const std::string& det_model_path,
                  const std::string& rec_model_path,
                  const std::string& dict_path = "");
    ~PpocrStrategy() override;

    bool Init() override;
    void Deinit() override;
    ImageBuffer ProcessFrame(const ImageBuffer& frame, NPU* npu) override;
    const char* GetName() const override { return "PPOCR"; }

private:
    std::string det_model_path_;
    std::string rec_model_path_;
    std::string dict_path_;
    std::unique_ptr<PPOCR> ppocr_;
};

} // namespace media
