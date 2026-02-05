/**
 * @file yolov5_model.cpp
 * @brief YOLOv5 模型实现
 *
 * 基于 Luckfox RKMPI 例程移植，适配 aipc 项目架构
 *
 * @author 好软，好温暖
 * @date 2026-02-05
 */

#define LOG_TAG "YoloV5"

#include "yolov5_model.h"
#include "common/logger.h"

#include <cstring>
#include <cmath>
#include <fstream>
#include <set>

namespace rknn {

// ============================================================================
// 常量定义
// ============================================================================

/// YOLOv5 Anchor 配置
static const int kAnchors[3][6] = {
    {10, 13, 16, 30, 33, 23},      // P3/8
    {30, 61, 62, 45, 59, 119},     // P4/16  
    {116, 90, 156, 198, 373, 326}  // P5/32
};

/// 每个位置的属性数量 (x, y, w, h, obj_conf, class_probs...)
static const int kPropBoxSize = 5 + kCocoClassNum;

// ============================================================================
// 辅助函数
// ============================================================================

namespace {

/// Clamp 函数
inline int Clamp(float val, int min_val, int max_val) {
    return val > min_val ? (val < max_val ? static_cast<int>(val) : max_val) : min_val;
}

/// 反量化：int8 -> float32
inline float DeqntAffineToF32(int8_t qnt, int32_t zp, float scale) {
    return (static_cast<float>(qnt) - static_cast<float>(zp)) * scale;
}

/// 量化：float32 -> int8
inline int8_t QntF32ToAffine(float f32, int32_t zp, float scale) {
    float dst_val = (f32 / scale) + zp;
    float clamped = dst_val <= -128 ? -128 : (dst_val >= 127 ? 127 : dst_val);
    return static_cast<int8_t>(clamped);
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
void QuickSortIndicesDescending(std::vector<float>& scores, int left, int right,
                                std::vector<int>& indices) {
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

/// NMS 实现
int NMS(int valid_count, std::vector<float>& boxes, std::vector<int>& class_ids,
        std::vector<int>& order, int filter_id, float threshold) {
    for (int i = 0; i < valid_count; ++i) {
        if (order[i] == -1 || class_ids[order[i]] != filter_id) continue;
        
        int n = order[i];
        for (int j = i + 1; j < valid_count; ++j) {
            int m = order[j];
            if (m == -1 || class_ids[m] != filter_id) continue;
            
            float x1_min = boxes[n * 4 + 0];
            float y1_min = boxes[n * 4 + 1];
            float x1_max = x1_min + boxes[n * 4 + 2];
            float y1_max = y1_min + boxes[n * 4 + 3];
            
            float x2_min = boxes[m * 4 + 0];
            float y2_min = boxes[m * 4 + 1];
            float x2_max = x2_min + boxes[m * 4 + 2];
            float y2_max = y2_min + boxes[m * 4 + 3];
            
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
// YoloV5Model 实现
// ============================================================================

YoloV5Model::YoloV5Model() {
    LOG_DEBUG("YoloV5Model created");
}

YoloV5Model::~YoloV5Model() {
    Deinit();
    LOG_DEBUG("YoloV5Model destroyed");
}

int YoloV5Model::Init(const ModelConfig& config) {
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
    input_attrs_ = new rknn_tensor_attr[io_num_.n_input];
    std::memset(input_attrs_, 0, io_num_.n_input * sizeof(rknn_tensor_attr));
    for (uint32_t i = 0; i < io_num_.n_input; ++i) {
        input_attrs_[i].index = i;
        ret = rknn_query(ctx_, RKNN_QUERY_NATIVE_INPUT_ATTR, &input_attrs_[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            LOG_ERROR("rknn_query NATIVE_INPUT_ATTR failed: ret={}", ret);
            return -1;
        }
    }
    
    // 4. 查询输出张量属性
    output_attrs_ = new rknn_tensor_attr[io_num_.n_output];
    std::memset(output_attrs_, 0, io_num_.n_output * sizeof(rknn_tensor_attr));
    for (uint32_t i = 0; i < io_num_.n_output; ++i) {
        output_attrs_[i].index = i;
        ret = rknn_query(ctx_, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR, &output_attrs_[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            LOG_ERROR("rknn_query NATIVE_OUTPUT_ATTR failed: ret={}", ret);
            return -1;
        }
    }
    
    // 5. 设置输入类型为 UINT8，使用 NHWC 格式（RV1106 零拷贝模式要求）
    input_attrs_[0].type = RKNN_TENSOR_UINT8;
    input_attrs_[0].fmt = RKNN_TENSOR_NHWC;
    
    // 6. 创建输入内存
    input_mem_ = rknn_create_mem(ctx_, input_attrs_[0].size_with_stride);
    if (!input_mem_) {
        LOG_ERROR("rknn_create_mem for input failed");
        return -1;
    }
    
    ret = rknn_set_io_mem(ctx_, input_mem_, &input_attrs_[0]);
    if (ret < 0) {
        LOG_ERROR("rknn_set_io_mem for input failed: ret={}", ret);
        return -1;
    }
    
    // 7. 创建输出内存
    for (uint32_t i = 0; i < io_num_.n_output; ++i) {
        output_mems_[i] = rknn_create_mem(ctx_, output_attrs_[i].size_with_stride);
        if (!output_mems_[i]) {
            LOG_ERROR("rknn_create_mem for output[{}] failed", i);
            return -1;
        }
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
    
    // 10. 加载标签文件（如果提供）
    if (!config.labels_path.empty()) {
        if (LoadLabels(config.labels_path) != 0) {
            LOG_WARN("Failed to load labels from: {}", config.labels_path);
        }
    }
    
    initialized_ = true;
    LOG_INFO("YOLOv5 model initialized successfully");
    return 0;
}

void YoloV5Model::Deinit() {
    if (!initialized_ && ctx_ == 0) return;
    
    LOG_INFO("Deinitializing YOLOv5 model...");
    
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
    
    labels_.clear();
    initialized_ = false;
    LOG_INFO("YOLOv5 model deinitialized");
}

bool YoloV5Model::IsInitialized() const {
    return initialized_;
}

int YoloV5Model::SetInput(const void* data, int width, int height, int stride) {
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
        // TODO: 实现 RGA 缩放
        return -1;
    }
    
    // 计算实际步长
    int actual_stride = (stride == 0) ? width * model_channel_ : stride;
    int expected_size = model_height_ * model_width_ * model_channel_;
    
    // 拷贝数据到输入内存
    if (actual_stride == model_width_ * model_channel_) {
        // 紧凑排列，直接拷贝
        std::memcpy(input_mem_->virt_addr, data, expected_size);
    } else {
        // 逐行拷贝
        uint8_t* dst = static_cast<uint8_t*>(input_mem_->virt_addr);
        const uint8_t* src = static_cast<const uint8_t*>(data);
        int row_size = model_width_ * model_channel_;
        for (int y = 0; y < model_height_; ++y) {
            std::memcpy(dst + y * row_size, src + y * actual_stride, row_size);
        }
    }
    
    return 0;
}

int YoloV5Model::SetInputDma(int dma_fd, int size, int width, int height) {
    if (!initialized_) {
        LOG_ERROR("Model not initialized");
        return -1;
    }
    
    // TODO: 实现 DMA Buffer 零拷贝输入
    // 需要使用 rknn_set_io_mem 重新设置输入内存
    LOG_WARN("SetInputDma not implemented yet, falling back to copy mode");
    (void)dma_fd;
    (void)size;
    (void)width;
    (void)height;
    return -1;
}

int YoloV5Model::Run() {
    if (!initialized_) {
        LOG_ERROR("Model not initialized");
        return -1;
    }
    
    int ret = rknn_run(ctx_, nullptr);
    if (ret < 0) {
        LOG_ERROR("rknn_run failed: ret={}", ret);
        return -1;
    }
    
    return 0;
}

int YoloV5Model::GetResults(DetectionResultList& results) {
    if (!initialized_) {
        LOG_ERROR("Model not initialized");
        return -1;
    }
    
    results.Clear();
    return PostProcess(results);
}

int YoloV5Model::PostProcess(DetectionResultList& results) {
    std::vector<float> boxes;
    std::vector<float> scores;
    std::vector<int> class_ids;
    
    // 不同尺度的 grid 大小和 stride
    const int strides[3] = {8, 16, 32};
    int valid_count = 0;
    
    // 遍历三个输出层
    for (uint32_t i = 0; i < io_num_.n_output && i < 3; ++i) {
        int grid_h = model_height_ / strides[i];
        int grid_w = model_width_ / strides[i];
        
        int8_t* output = static_cast<int8_t*>(output_mems_[i]->virt_addr);
        int32_t zp = output_attrs_[i].zp;
        float scale = output_attrs_[i].scale;
        
        int8_t threshold_i8 = QntF32ToAffine(config_.conf_threshold, zp, scale);
        
        // RV1106 NHWC 格式处理
        int anchor_per_branch = 3;
        int align_c = kPropBoxSize * anchor_per_branch;
        
        for (int h = 0; h < grid_h; ++h) {
            for (int w = 0; w < grid_w; ++w) {
                for (int a = 0; a < anchor_per_branch; ++a) {
                    int hw_offset = h * grid_w * align_c + w * align_c + a * kPropBoxSize;
                    int8_t* ptr = output + hw_offset;
                    int8_t box_conf = ptr[4];
                    
                    if (box_conf >= threshold_i8) {
                        // 找到最大类别概率
                        int8_t max_class_prob = ptr[5];
                        int max_class_id = 0;
                        for (int k = 1; k < kCocoClassNum; ++k) {
                            if (ptr[5 + k] > max_class_prob) {
                                max_class_prob = ptr[5 + k];
                                max_class_id = k;
                            }
                        }
                        
                        float box_conf_f32 = DeqntAffineToF32(box_conf, zp, scale);
                        float class_prob_f32 = DeqntAffineToF32(max_class_prob, zp, scale);
                        float final_score = box_conf_f32 * class_prob_f32;
                        
                        if (final_score > config_.conf_threshold) {
                            // 解码边界框
                            float box_x = DeqntAffineToF32(ptr[0], zp, scale) * 2.0f - 0.5f;
                            float box_y = DeqntAffineToF32(ptr[1], zp, scale) * 2.0f - 0.5f;
                            float box_w = DeqntAffineToF32(ptr[2], zp, scale) * 2.0f;
                            float box_h = DeqntAffineToF32(ptr[3], zp, scale) * 2.0f;
                            
                            box_w = box_w * box_w;
                            box_h = box_h * box_h;
                            
                            box_x = (box_x + w) * strides[i];
                            box_y = (box_y + h) * strides[i];
                            box_w *= kAnchors[i][a * 2];
                            box_h *= kAnchors[i][a * 2 + 1];
                            
                            // 转换为左上角坐标
                            box_x -= box_w / 2.0f;
                            box_y -= box_h / 2.0f;
                            
                            boxes.push_back(box_x);
                            boxes.push_back(box_y);
                            boxes.push_back(box_w);
                            boxes.push_back(box_h);
                            scores.push_back(final_score);
                            class_ids.push_back(max_class_id);
                            valid_count++;
                        }
                    }
                }
            }
        }
    }
    
    if (valid_count == 0) {
        return 0;
    }
    
    // NMS
    std::vector<int> order(valid_count);
    for (int i = 0; i < valid_count; ++i) order[i] = i;
    
    // 按分数排序
    std::vector<float> scores_copy = scores;
    QuickSortIndicesDescending(scores_copy, 0, valid_count - 1, order);
    
    // 对每个类别执行 NMS
    std::set<int> unique_classes(class_ids.begin(), class_ids.end());
    for (int cls_id : unique_classes) {
        NMS(valid_count, boxes, class_ids, order, cls_id, config_.nms_threshold);
    }
    
    // 收集最终结果
    for (int i = 0; i < valid_count && results.Count() < static_cast<size_t>(config_.max_detections); ++i) {
        int idx = order[i];
        if (idx == -1) continue;
        
        DetectionResult det;
        det.class_id = class_ids[idx];
        det.confidence = scores[idx];
        det.box.x = Clamp(boxes[idx * 4 + 0], 0, model_width_);
        det.box.y = Clamp(boxes[idx * 4 + 1], 0, model_height_);
        det.box.width = Clamp(boxes[idx * 4 + 2], 0, model_width_ - det.box.x);
        det.box.height = Clamp(boxes[idx * 4 + 3], 0, model_height_ - det.box.y);
        
        // 设置标签
        if (det.class_id >= 0 && det.class_id < static_cast<int>(labels_.size())) {
            det.label = labels_[det.class_id];
        } else {
            det.label = "class_" + std::to_string(det.class_id);
        }
        
        results.Add(det);
    }
    
    return 0;
}

int YoloV5Model::LoadLabels(const std::string& labels_path) {
    std::ifstream file(labels_path);
    if (!file.is_open()) {
        LOG_ERROR("Failed to open labels file: {}", labels_path);
        return -1;
    }
    
    labels_.clear();
    std::string line;
    while (std::getline(file, line)) {
        // 去除行尾空白
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) {
            labels_.push_back(line);
        }
    }
    
    LOG_INFO("Loaded {} labels from {}", labels_.size(), labels_path);
    return 0;
}

ModelInfo YoloV5Model::GetModelInfo() const {
    ModelInfo info;
    info.input_width = model_width_;
    info.input_height = model_height_;
    info.input_channels = model_channel_;
    info.is_quant = is_quant_;
    info.type = ModelType::kYoloV5;
    return info;
}

void YoloV5Model::GetInputSize(int& width, int& height) const {
    width = model_width_;
    height = model_height_;
}

void* YoloV5Model::GetInputVirtAddr() const {
    return input_mem_ ? input_mem_->virt_addr : nullptr;
}

int YoloV5Model::GetInputMemSize() const {
    return input_mem_ ? input_mem_->size : 0;
}

}  // namespace rknn
