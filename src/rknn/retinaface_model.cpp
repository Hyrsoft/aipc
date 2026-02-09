/**
 * @file retinaface_model.cpp
 * @brief RetinaFace 模型实现
 *
 * 基于 Luckfox RKMPI 例程移植，适配 aipc 项目架构
 *
 * @author 好软，好温暖
 * @date 2026-02-05
 */

#define LOG_TAG "RetinaFace"

#include "retinaface_model.h"
#include "rkvideo/osd_overlay.h"
#include "common/logger.h"

#include <cstring>
#include <cmath>

// 引入 Luckfox 提供的 box priors
#include "rknn_box_priors.h"

namespace rknn {

// ============================================================================
// 辅助函数
// ============================================================================

namespace {

/// Clamp 函数
inline int Clamp(float val, int min_val, int max_val) {
    return val > min_val ? (val < max_val ? static_cast<int>(val) : max_val) : min_val;
}

/// 从 uint8 反量化（RetinaFace 使用 uint8 输出）
inline float DeqntAffineToF32_U8(uint8_t qnt, int32_t zp, float scale) {
    return (static_cast<float>(qnt) - static_cast<float>(zp)) * scale;
}

/// 计算 IoU
float CalculateIoU(float x1_min, float y1_min, float x1_max, float y1_max,
                   float x2_min, float y2_min, float x2_max, float y2_max) {
    float w = std::fmax(0.f, std::fmin(x1_max, x2_max) - std::fmax(x1_min, x2_min) + 1.0f);
    float h = std::fmax(0.f, std::fmin(y1_max, y2_max) - std::fmax(y1_min, y2_min) + 1.0f);
    float intersection = w * h;
    float union_area = (x1_max - x1_min + 1.0f) * (y1_max - y1_min + 1.0f) +
                       (x2_max - x2_min + 1.0f) * (y2_max - y2_min + 1.0f) - intersection;
    return union_area <= 0.f ? 0.f : (intersection / union_area);
}

/// NMS 排序辅助
void QuickSortIndicesDescending(float* scores, int left, int right, int* indices) {
    if (left >= right) return;
    
    float pivot = scores[left];
    int pivot_idx = indices[left];
    int low = left, high = right;
    
    while (low < high) {
        while (low < high && scores[high] <= pivot) high--;
        scores[low] = scores[high];
        indices[low] = indices[high];
        
        while (low < high && scores[low] >= pivot) low++;
        scores[high] = scores[low];
        indices[high] = indices[low];
    }
    scores[low] = pivot;
    indices[low] = pivot_idx;
    
    QuickSortIndicesDescending(scores, left, low - 1, indices);
    QuickSortIndicesDescending(scores, low + 1, right, indices);
}

/// NMS 实现（RetinaFace 版本，基于归一化坐标）
int NMSRetinaFace(int valid_count, float* locations, int* order, float threshold,
                   int width, int height) {
    for (int i = 0; i < valid_count; ++i) {
        if (order[i] == -1) continue;
        
        int n = order[i];
        for (int j = i + 1; j < valid_count; ++j) {
            int m = order[j];
            if (m == -1) continue;
            
            float x1_min = locations[n * 4 + 0] * width;
            float y1_min = locations[n * 4 + 1] * height;
            float x1_max = locations[n * 4 + 2] * width;
            float y1_max = locations[n * 4 + 3] * height;
            
            float x2_min = locations[m * 4 + 0] * width;
            float y2_min = locations[m * 4 + 1] * height;
            float x2_max = locations[m * 4 + 2] * width;
            float y2_max = locations[m * 4 + 3] * height;
            
            float iou = CalculateIoU(x1_min, y1_min, x1_max, y1_max,
                                     x2_min, y2_min, x2_max, y2_max);
            if (iou > threshold) {
                order[j] = -1;
            }
        }
    }
    return 0;
}

}  // anonymous namespace

// ============================================================================
// RetinaFaceModel 实现
// ============================================================================

RetinaFaceModel::RetinaFaceModel() {
    LOG_DEBUG("RetinaFaceModel created");
}

RetinaFaceModel::~RetinaFaceModel() {
    Deinit();
    LOG_DEBUG("RetinaFaceModel destroyed");
}

int RetinaFaceModel::Init(const ModelConfig& config) {
    if (initialized_) {
        LOG_WARN("Model already initialized, deinitializing first");
        Deinit();
    }
    
    config_ = config;
    int ret = 0;
    
    // 1. 初始化 RKNN 上下文
    LOG_INFO("Loading RKNN model: {}", config.model_path);
    ret = rknn_init(&ctx_, const_cast<char*>(config.model_path.c_str()), 0, 0, nullptr);
    if (ret < 0) {
        LOG_ERROR("rknn_init failed: ret={}", ret);
        return -1;
    }
    
    // 2. 查询输入输出数量
    ret = rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num_, sizeof(io_num_));
    if (ret != RKNN_SUCC) {
        LOG_ERROR("rknn_query IN_OUT_NUM failed: ret={}", ret);
        return -1;
    }
    LOG_INFO("Model input num: {}, output num: {}", io_num_.n_input, io_num_.n_output);
    
    // 3. 查询输入张量属性
    LOG_INFO("Input tensors:");
    input_attrs_ = new rknn_tensor_attr[io_num_.n_input];
    std::memset(input_attrs_, 0, io_num_.n_input * sizeof(rknn_tensor_attr));
    for (uint32_t i = 0; i < io_num_.n_input; ++i) {
        input_attrs_[i].index = i;
        ret = rknn_query(ctx_, RKNN_QUERY_NATIVE_INPUT_ATTR, &input_attrs_[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            LOG_ERROR("rknn_query NATIVE_INPUT_ATTR failed: ret={}", ret);
            return -1;
        }
        LOG_INFO("  [{}] index={}, n_dims={}, dims=[{}, {}, {}, {}], n_elems={}, size={}, size_with_stride={}, "
                 "fmt={}, type={}, qnt_type={}, zp={}, scale={}",
                 i, input_attrs_[i].index, input_attrs_[i].n_dims,
                 input_attrs_[i].dims[0], input_attrs_[i].dims[1], 
                 input_attrs_[i].dims[2], input_attrs_[i].dims[3],
                 input_attrs_[i].n_elems, input_attrs_[i].size, input_attrs_[i].size_with_stride,
                 (int)input_attrs_[i].fmt, (int)input_attrs_[i].type,
                 (int)input_attrs_[i].qnt_type, input_attrs_[i].zp, input_attrs_[i].scale);
    }
    
    // 4. 查询输出张量属性（RetinaFace 有 3 个输出：location, scores, landmarks）
    LOG_INFO("Output tensors:");
    output_attrs_ = new rknn_tensor_attr[io_num_.n_output];
    std::memset(output_attrs_, 0, io_num_.n_output * sizeof(rknn_tensor_attr));
    for (uint32_t i = 0; i < io_num_.n_output; ++i) {
        output_attrs_[i].index = i;
        ret = rknn_query(ctx_, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR, &output_attrs_[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            LOG_ERROR("rknn_query NATIVE_OUTPUT_ATTR failed: ret={}", ret);
            return -1;
        }
        LOG_INFO("  [{}] index={}, n_dims={}, dims=[{}, {}, {}, {}], n_elems={}, size={}, size_with_stride={}, "
                 "fmt={}, type={}, qnt_type={}, zp={}, scale={}",
                 i, output_attrs_[i].index, output_attrs_[i].n_dims,
                 output_attrs_[i].dims[0], output_attrs_[i].dims[1], 
                 output_attrs_[i].dims[2], output_attrs_[i].dims[3],
                 output_attrs_[i].n_elems, output_attrs_[i].size, output_attrs_[i].size_with_stride,
                 (int)output_attrs_[i].fmt, (int)output_attrs_[i].type,
                 (int)output_attrs_[i].qnt_type, output_attrs_[i].zp, output_attrs_[i].scale);
    }
    
    // 5. 设置输入类型为 UINT8，使用 NHWC 格式
    input_attrs_[0].type = RKNN_TENSOR_UINT8;
    input_attrs_[0].fmt = RKNN_TENSOR_NHWC;
    LOG_INFO("Set input to UINT8/NHWC, size_with_stride={}", input_attrs_[0].size_with_stride);
    
    // 6. 创建输入内存
    input_mem_ = rknn_create_mem(ctx_, input_attrs_[0].size_with_stride);
    if (!input_mem_) {
        LOG_ERROR("rknn_create_mem for input failed");
        return -1;
    }
    LOG_INFO("Created input memory: virt_addr={}, size={}", input_mem_->virt_addr, input_mem_->size);
    
    ret = rknn_set_io_mem(ctx_, input_mem_, &input_attrs_[0]);
    if (ret < 0) {
        LOG_ERROR("rknn_set_io_mem for input failed: ret={}", ret);
        return -1;
    }
    LOG_INFO("Set input io_mem success");
    
    // 7. 创建输出内存
    for (uint32_t i = 0; i < io_num_.n_output && i < 3; ++i) {
        output_mems_[i] = rknn_create_mem(ctx_, output_attrs_[i].size_with_stride);
        if (!output_mems_[i]) {
            LOG_ERROR("rknn_create_mem for output[{}] failed", i);
            return -1;
        }
        LOG_INFO("Created output[{}] memory: virt_addr={}, size={}", 
                 i, output_mems_[i]->virt_addr, output_mems_[i]->size);
        
        ret = rknn_set_io_mem(ctx_, output_mems_[i], &output_attrs_[i]);
        if (ret < 0) {
            LOG_ERROR("rknn_set_io_mem for output[{}] failed: ret={}", i, ret);
            return -1;
        }
    }
    
    // 8. 解析模型尺寸
    if (input_attrs_[0].fmt == RKNN_TENSOR_NCHW) {
        model_channel_ = input_attrs_[0].dims[1];
        model_height_ = input_attrs_[0].dims[2];
        model_width_ = input_attrs_[0].dims[3];
    } else {  // NHWC
        model_height_ = input_attrs_[0].dims[1];
        model_width_ = input_attrs_[0].dims[2];
        model_channel_ = input_attrs_[0].dims[3];
    }
    LOG_INFO("Model input: {}x{}x{}", model_width_, model_height_, model_channel_);
    
    // 9. 检查是否为量化模型
    is_quant_ = (output_attrs_[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC);
    LOG_INFO("Model is {}", is_quant_ ? "quantized (INT8)" : "float");
    
    initialized_ = true;
    LOG_INFO("RetinaFace model initialized successfully");
    return 0;
}

void RetinaFaceModel::Deinit() {
    if (!initialized_ && ctx_ == 0) return;
    
    LOG_INFO("Deinitializing RetinaFace model...");
    
    // 释放输入内存
    if (input_mem_) {
        rknn_destroy_mem(ctx_, input_mem_);
        input_mem_ = nullptr;
    }
    
    // 释放输出内存
    for (int i = 0; i < 3; ++i) {
        if (output_mems_[i]) {
            rknn_destroy_mem(ctx_, output_mems_[i]);
            output_mems_[i] = nullptr;
        }
    }
    
    // 释放属性数组
    delete[] input_attrs_;
    input_attrs_ = nullptr;
    delete[] output_attrs_;
    output_attrs_ = nullptr;
    
    // 销毁 RKNN 上下文
    if (ctx_ != 0) {
        rknn_destroy(ctx_);
        ctx_ = 0;
    }
    
    initialized_ = false;
    LOG_INFO("RetinaFace model deinitialized");
}

bool RetinaFaceModel::IsInitialized() const {
    return initialized_;
}

int RetinaFaceModel::SetInput(const void* data, int width, int height, int stride) {
    if (!initialized_) {
        LOG_ERROR("Model not initialized");
        return -1;
    }
    
    if (!data) {
        LOG_ERROR("Input data is null");
        return -1;
    }
    
    // 检查尺寸是否匹配
    if (width != model_width_ || height != model_height_) {
        LOG_WARN("Input size {}x{} doesn't match model size {}x{}, need resize",
                 width, height, model_width_, model_height_);
        return -1;
    }
    
    // 计算实际步长
    int actual_stride = (stride == 0) ? width * model_channel_ : stride;
    int expected_size = model_height_ * model_width_ * model_channel_;
    
    // 拷贝数据到输入内存
    if (actual_stride == model_width_ * model_channel_) {
        std::memcpy(input_mem_->virt_addr, data, expected_size);
    } else {
        uint8_t* dst = static_cast<uint8_t*>(input_mem_->virt_addr);
        const uint8_t* src = static_cast<const uint8_t*>(data);
        int row_size = model_width_ * model_channel_;
        for (int y = 0; y < model_height_; ++y) {
            std::memcpy(dst + y * row_size, src + y * actual_stride, row_size);
        }
    }
    
    return 0;
}

int RetinaFaceModel::SetInputDma(int dma_fd, int size, int width, int height) {
    if (!initialized_) {
        LOG_ERROR("Model not initialized");
        return -1;
    }
    
    LOG_WARN("SetInputDma not implemented yet");
    (void)dma_fd;
    (void)size;
    (void)width;
    (void)height;
    return -1;
}

int RetinaFaceModel::Run() {
    if (!initialized_) {
        LOG_ERROR("Model not initialized");
        return -1;
    }
    
    LOG_DEBUG("Calling rknn_run...");
    int ret = rknn_run(ctx_, nullptr);
    if (ret < 0) {
        LOG_ERROR("rknn_run failed: ret={}", ret);
        return -1;
    }
    LOG_DEBUG("rknn_run completed successfully");
    
    return 0;
}

int RetinaFaceModel::GetResults(DetectionResultList& results) {
    if (!initialized_) {
        LOG_ERROR("Model not initialized");
        return -1;
    }
    
    results.Clear();
    return PostProcess(results);
}

int RetinaFaceModel::PostProcess(DetectionResultList& results) {
    // 获取输出指针
    uint8_t* location = static_cast<uint8_t*>(output_mems_[0]->virt_addr);
    uint8_t* scores = static_cast<uint8_t*>(output_mems_[1]->virt_addr);
    uint8_t* landmarks = static_cast<uint8_t*>(output_mems_[2]->virt_addr);
    
    // 选择对应尺寸的 prior boxes
    const float (*prior_ptr)[4];
    int num_priors;
    if (model_width_ == 640) {
        prior_ptr = BOX_PRIORS_640;
        num_priors = 16800;
    } else if (model_width_ == 320) {
        prior_ptr = BOX_PRIORS_320;
        num_priors = 4200;
    } else {
        LOG_ERROR("Unsupported model size: {}", model_width_);
        return -1;
    }
    
    // 分配临时数组
    std::vector<int> filter_indices(num_priors);
    std::vector<float> props(num_priors, 0.0f);
    std::vector<float> loc_fp32(num_priors * 4, 0.0f);
    std::vector<float> landms_fp32(num_priors * 10, 0.0f);
    
    // 量化参数
    int32_t loc_zp = output_attrs_[0].zp;
    float loc_scale = output_attrs_[0].scale;
    int32_t scores_zp = output_attrs_[1].zp;
    float scores_scale = output_attrs_[1].scale;
    int32_t landms_zp = output_attrs_[2].zp;
    float landms_scale = output_attrs_[2].scale;
    
    // 方差参数（RetinaFace 固定值）
    const float VARIANCES[2] = {0.1f, 0.2f};
    
    int valid_count = 0;
    float conf_threshold = config_.conf_threshold > 0 ? config_.conf_threshold : 0.5f;
    
    // 解码 boxes
    for (int i = 0; i < num_priors && valid_count < kMaxDetections; ++i) {
        // scores 输出格式：[num_priors, 2]，第二个值为人脸置信度
        float face_score = DeqntAffineToF32_U8(scores[i * 2 + 1], scores_zp, scores_scale);
        
        if (face_score > conf_threshold) {
            filter_indices[valid_count] = i;
            props[valid_count] = face_score;
            
            int offset = i * 4;
            uint8_t* bbox = location + offset;
            
            // 解码边界框
            float box_x = DeqntAffineToF32_U8(bbox[0], loc_zp, loc_scale) * VARIANCES[0] * prior_ptr[i][2] + prior_ptr[i][0];
            float box_y = DeqntAffineToF32_U8(bbox[1], loc_zp, loc_scale) * VARIANCES[0] * prior_ptr[i][3] + prior_ptr[i][1];
            float box_w = std::exp(DeqntAffineToF32_U8(bbox[2], loc_zp, loc_scale) * VARIANCES[1]) * prior_ptr[i][2];
            float box_h = std::exp(DeqntAffineToF32_U8(bbox[3], loc_zp, loc_scale) * VARIANCES[1]) * prior_ptr[i][3];
            
            float xmin = box_x - box_w * 0.5f;
            float ymin = box_y - box_h * 0.5f;
            float xmax = xmin + box_w;
            float ymax = ymin + box_h;
            
            loc_fp32[offset + 0] = xmin;
            loc_fp32[offset + 1] = ymin;
            loc_fp32[offset + 2] = xmax;
            loc_fp32[offset + 3] = ymax;
            
            // 解码关键点
            for (int j = 0; j < 5; ++j) {
                landms_fp32[i * 10 + 2 * j] = DeqntAffineToF32_U8(landmarks[i * 10 + 2 * j], landms_zp, landms_scale)
                                              * VARIANCES[0] * prior_ptr[i][2] + prior_ptr[i][0];
                landms_fp32[i * 10 + 2 * j + 1] = DeqntAffineToF32_U8(landmarks[i * 10 + 2 * j + 1], landms_zp, landms_scale)
                                                  * VARIANCES[0] * prior_ptr[i][3] + prior_ptr[i][1];
            }
            
            ++valid_count;
        }
    }
    
    if (valid_count == 0) {
        return 0;
    }
    
    // 排序
    QuickSortIndicesDescending(props.data(), 0, valid_count - 1, filter_indices.data());
    
    // NMS
    float nms_threshold = config_.nms_threshold > 0 ? config_.nms_threshold : 0.2f;
    NMSRetinaFace(valid_count, loc_fp32.data(), filter_indices.data(), nms_threshold, model_width_, model_height_);
    
    // 收集结果
    for (int i = 0; i < valid_count && results.Count() < static_cast<size_t>(config_.max_detections); ++i) {
        if (filter_indices[i] == -1 || props[i] < conf_threshold) {
            continue;
        }
        
        int n = filter_indices[i];
        
        DetectionResult det;
        det.class_id = 0;  // 人脸类别固定为 0
        det.confidence = props[i];
        det.label = "face";
        
        // 转换为像素坐标
        float x1 = loc_fp32[n * 4 + 0] * model_width_;
        float y1 = loc_fp32[n * 4 + 1] * model_height_;
        float x2 = loc_fp32[n * 4 + 2] * model_width_;
        float y2 = loc_fp32[n * 4 + 3] * model_height_;
        
        det.box.x = Clamp(x1, 0, model_width_);
        det.box.y = Clamp(y1, 0, model_height_);
        det.box.width = Clamp(x2 - x1, 0, model_width_ - det.box.x);
        det.box.height = Clamp(y2 - y1, 0, model_height_ - det.box.y);
        
        // 添加关键点
        det.landmarks.resize(5);
        for (int j = 0; j < 5; ++j) {
            float point_x = landms_fp32[n * 10 + 2 * j] * model_width_;
            float point_y = landms_fp32[n * 10 + 2 * j + 1] * model_height_;
            det.landmarks[j].x = Clamp(point_x, 0, model_width_);
            det.landmarks[j].y = Clamp(point_y, 0, model_height_);
        }
        
        results.Add(det);
    }
    
    return 0;
}

ModelInfo RetinaFaceModel::GetModelInfo() const {
    ModelInfo info;
    info.input_width = model_width_;
    info.input_height = model_height_;
    info.input_channels = model_channel_;
    info.is_quant = is_quant_;
    info.type = ModelType::kRetinaFace;
    return info;
}

void RetinaFaceModel::GetInputSize(int& width, int& height) const {
    width = model_width_;
    height = model_height_;
}

void* RetinaFaceModel::GetInputVirtAddr() const {
    return input_mem_ ? input_mem_->virt_addr : nullptr;
}

int RetinaFaceModel::GetInputMemSize() const {
    return input_mem_ ? input_mem_->size : 0;
}

std::string RetinaFaceModel::FormatResultLog(const DetectionResult& result, size_t index,
                                              float letterbox_scale,
                                              int letterbox_pad_x,
                                              int letterbox_pad_y) const {
    // RetinaFace 特化格式：显示人脸框和关键点
    int x1 = static_cast<int>((result.box.x - letterbox_pad_x) / letterbox_scale);
    int y1 = static_cast<int>((result.box.y - letterbox_pad_y) / letterbox_scale);
    int x2 = static_cast<int>((result.box.x + result.box.width - letterbox_pad_x) / letterbox_scale);
    int y2 = static_cast<int>((result.box.y + result.box.height - letterbox_pad_y) / letterbox_scale);
    
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "[%zu] face @ (%d,%d,%d,%d) conf=%.1f%%",
                       index, x1, y1, x2, y2, result.confidence * 100);
    
    // 添加关键点信息（如果有）
    if (result.HasLandmarks() && result.landmarks.size() >= 5) {
        len += snprintf(buf + len, sizeof(buf) - len, " lm:[");
        for (size_t i = 0; i < result.landmarks.size() && i < 5; ++i) {
            int lx = static_cast<int>((result.landmarks[i].x - letterbox_pad_x) / letterbox_scale);
            int ly = static_cast<int>((result.landmarks[i].y - letterbox_pad_y) / letterbox_scale);
            len += snprintf(buf + len, sizeof(buf) - len, "%s(%d,%d)",
                           i > 0 ? "," : "", lx, ly);
        }
        snprintf(buf + len, sizeof(buf) - len, "]");
    }
    
    return std::string(buf);
}

void RetinaFaceModel::GenerateOSDBoxes(const DetectionResultList& results,
                                        std::vector<OSDBox>& boxes) const {
    // RetinaFace 特化：人脸统一使用黄色框
    static const uint32_t kFaceColor = 0xFFFF00FF;  // 黄色
    
    boxes.clear();
    for (size_t i = 0; i < results.Count(); ++i) {
        const auto& det = results.results[i];
        OSDBox box;
        box.x = det.box.x;
        box.y = det.box.y;
        box.width = det.box.width;
        box.height = det.box.height;
        box.label_id = 0;  // 人脸类别固定为 0
        box.color = kFaceColor;
        boxes.push_back(box);
    }
    
    // TODO: 未来可以扩展支持关键点显示
    // 但需要对 OSD 模块进行扩展，支持点绘制
}

}  // namespace rknn
