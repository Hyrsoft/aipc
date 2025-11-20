#include "RetinaFaceEngine.hpp"
#include "rknn_box_priors.h"
#include <iostream>
#include <opencv2/imgproc.hpp>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "rknn_api.h"

// Define internal structures to avoid conflicts with YOLOv5
typedef struct {
    int left;
    int top;
    int right;
    int bottom;
} rf_image_rect_t; 

typedef struct {
    int x;
    int y;
} rf_point_t;

typedef struct {
    rf_image_rect_t box;
    float prop;
    rf_point_t point[5];
} rf_object_detect_result;

typedef struct {
    int count;
    rf_object_detect_result results[128];
} rf_object_detect_result_list;

typedef struct {
    rknn_context rknn_ctx;
    rknn_tensor_mem* max_mem;
    rknn_tensor_mem* net_mem;
    rknn_input_output_num io_num;
    rknn_tensor_attr* input_attrs;
    rknn_tensor_attr* output_attrs;
    rknn_tensor_mem* input_mems[1];
    rknn_tensor_mem* output_mems[3];
    int model_channel;
    int model_width;
    int model_height;
    bool is_quant;
} rf_app_context_t;

// Helper functions
static void dump_tensor_attr(rknn_tensor_attr *attr)
{
    printf("  index=%d, name=%s, n_dims=%d, dims=[%d, %d, %d, %d], n_elems=%d, size=%d, fmt=%s, type=%s, qnt_type=%s, "
           "zp=%d, scale=%f\n",
           attr->index, attr->name, attr->n_dims, attr->dims[0], attr->dims[1], attr->dims[2], attr->dims[3],
           attr->n_elems, attr->size, get_format_string(attr->fmt), get_type_string(attr->type),
           get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

static float CalculateOverlap(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1, float ymin1, float xmax1, float ymax1) {
    float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1);
    float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1);
    float i = w * h;
    float u = (xmax0 - xmin0 + 1) * (ymax0 - ymin0 + 1) + (xmax1 - xmin1 + 1) * (ymax1 - ymin1 + 1) - i;
    return u <= 0.f ? 0.f : (i / u);
}

static int quick_sort_indice_inverse(float *input, int left, int right, int *indices) {
    float key;
    int key_index;
    int low = left;
    int high = right;
    if (left < right) {
        key_index = indices[left];
        key = input[left];
        while (low < high) {
            while (low < high && input[high] <= key) {
                high--;
            }
            input[low] = input[high];
            indices[low] = indices[high];
            while (low < high && input[low] >= key) {
                low++;
            }
            input[high] = input[low];
            indices[high] = indices[low];
        }
        input[low] = key;
        indices[low] = key_index;
        quick_sort_indice_inverse(input, left, low - 1, indices);
        quick_sort_indice_inverse(input, low + 1, right, indices);
    }
    return low;
}

static int nms(int validCount, float *outputLocations, int order[], float threshold, int width, int height) {
    for (int i = 0; i < validCount; ++i) {
        if (order[i] == -1) {
            continue;
        }
        int n = order[i];
        for (int j = i + 1; j < validCount; ++j) {
            int m = order[j];
            if (m == -1) {
                continue;
            }
            float xmin0 = outputLocations[n * 4 + 0] * width;
            float ymin0 = outputLocations[n * 4 + 1] * height;
            float xmax0 = outputLocations[n * 4 + 2] * width;
            float ymax0 = outputLocations[n * 4 + 3] * height;

            float xmin1 = outputLocations[m * 4 + 0] * width;
            float ymin1 = outputLocations[m * 4 + 1] * height;
            float xmax1 = outputLocations[m * 4 + 2] * width;
            float ymax1 = outputLocations[m * 4 + 3] * height;

            float iou = CalculateOverlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1);

            if (iou > threshold) {
                order[j] = -1;
            }
        }
    }
    return 0;
}

inline static int32_t __clip(float val, float min, float max)
{
    float f = val <= min ? min : (val >= max ? max : val);
    return f;
}

static int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale)
{
    float dst_val = (f32 / scale) + zp;
    int8_t res = (int8_t)__clip(dst_val, -128, 127);
    return res;
}

static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale) { return ((float)qnt - (float)zp) * scale; }

static int clamp(float x, int min, int max) {
    if (x > max) return max;
    if (x < min) return min;
    return x;
}

struct RetinaFaceContext {
    rf_app_context_t app_ctx;
};

RetinaFaceEngine::RetinaFaceEngine() : ctx_(std::make_unique<RetinaFaceContext>()) {
    memset(&ctx_->app_ctx, 0, sizeof(rf_app_context_t));
}

RetinaFaceEngine::~RetinaFaceEngine() {
    if (ctx_->app_ctx.rknn_ctx != 0) {
        rknn_destroy(ctx_->app_ctx.rknn_ctx);
        ctx_->app_ctx.rknn_ctx = 0;
    }
    if (ctx_->app_ctx.input_attrs != NULL) {
        free(ctx_->app_ctx.input_attrs);
        ctx_->app_ctx.input_attrs = NULL;
    }
    if (ctx_->app_ctx.output_attrs != NULL) {
        free(ctx_->app_ctx.output_attrs);
        ctx_->app_ctx.output_attrs = NULL;
    }
    for (int i = 0; i < ctx_->app_ctx.io_num.n_input; i++) {
        if (ctx_->app_ctx.input_mems[i] != NULL) {
            rknn_destroy_mem(ctx_->app_ctx.rknn_ctx, ctx_->app_ctx.input_mems[i]);
        }
    }
    for (int i = 0; i < ctx_->app_ctx.io_num.n_output; i++) {
        if (ctx_->app_ctx.output_mems[i] != NULL) {
            rknn_destroy_mem(ctx_->app_ctx.rknn_ctx, ctx_->app_ctx.output_mems[i]);
        }
    }
}

int RetinaFaceEngine::Init(const std::string& model_path) {
    int ret;
    rknn_context ctx = 0;

    ret = rknn_init(&ctx, (char *)model_path.c_str(), 0, 0, NULL);
    if (ret < 0) {
        printf("rknn_init fail! ret=%d\n", ret);
        return -1;
    }

    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        printf("rknn_query fail! ret=%d\n", ret);
        return -1;
    }

    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (int i = 0; i < io_num.n_input; i++) {
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&(input_attrs[i]));
    }

    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (int i = 0; i < io_num.n_output; i++) {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&(output_attrs[i]));
    }

    input_attrs[0].type = RKNN_TENSOR_UINT8;
    input_attrs[0].fmt = RKNN_TENSOR_NHWC;
    ctx_->app_ctx.input_mems[0] = rknn_create_mem(ctx, input_attrs[0].size_with_stride);

    ret = rknn_set_io_mem(ctx, ctx_->app_ctx.input_mems[0], &input_attrs[0]);
    if (ret < 0) {
        printf("input_mems rknn_set_io_mem fail! ret=%d\n", ret);
        return -1;
    }

    for (uint32_t i = 0; i < io_num.n_output; ++i) {
        ctx_->app_ctx.output_mems[i] = rknn_create_mem(ctx, output_attrs[i].size_with_stride);
        ret = rknn_set_io_mem(ctx, ctx_->app_ctx.output_mems[i], &output_attrs[i]);
        if (ret < 0) {
            printf("output_mems rknn_set_io_mem fail! ret=%d\n", ret);
            return -1;
        }
    }

    ctx_->app_ctx.rknn_ctx = ctx;

    if (output_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) {
        ctx_->app_ctx.is_quant = true;
    } else {
        ctx_->app_ctx.is_quant = false;
    }

    ctx_->app_ctx.io_num = io_num;
    ctx_->app_ctx.input_attrs = (rknn_tensor_attr *)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
    memcpy(ctx_->app_ctx.input_attrs, input_attrs, io_num.n_input * sizeof(rknn_tensor_attr));
    ctx_->app_ctx.output_attrs = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
    memcpy(ctx_->app_ctx.output_attrs, output_attrs, io_num.n_output * sizeof(rknn_tensor_attr));

    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        ctx_->app_ctx.model_channel = input_attrs[0].dims[1];
        ctx_->app_ctx.model_height  = input_attrs[0].dims[2];
        ctx_->app_ctx.model_width   = input_attrs[0].dims[3];
    } else {
        ctx_->app_ctx.model_height  = input_attrs[0].dims[1];
        ctx_->app_ctx.model_width   = input_attrs[0].dims[2];
        ctx_->app_ctx.model_channel = input_attrs[0].dims[3];
    }

    return 0;
}

int RetinaFaceEngine::Inference(const cv::Mat& img, std::vector<ObjectDet>& results) {
    if (ctx_->app_ctx.rknn_ctx == 0) return -1;

    int width = ctx_->app_ctx.model_width;
    int height = ctx_->app_ctx.model_height;
    
    cv::Mat resized_img;
    cv::resize(img, resized_img, cv::Size(width, height));
    
    memcpy(ctx_->app_ctx.input_mems[0]->virt_addr, resized_img.data, width * height * 3);

    int ret = rknn_run(ctx_->app_ctx.rknn_ctx, nullptr);
    if (ret < 0) {
        printf("rknn_run fail! ret=%d\n", ret);
        return -1;
    }

    uint8_t *location =   (uint8_t *)(ctx_->app_ctx.output_mems[0]->virt_addr);
    uint8_t *scores   =   (uint8_t *)(ctx_->app_ctx.output_mems[1]->virt_addr);
    uint8_t *landms   =   (uint8_t *)(ctx_->app_ctx.output_mems[2]->virt_addr);

    const float (*prior_ptr)[4];
    int num_priors = 16800;
    prior_ptr = BOX_PRIORS_640;
    
    int filter_indices[num_priors];
    float props[num_priors]; 
    uint32_t location_size = ctx_->app_ctx.output_mems[0]->size; 
    uint32_t landms_size = ctx_->app_ctx.output_mems[2]->size; 
    float loc_fp32[location_size]; 
    float landms_fp32[landms_size];
    memset(loc_fp32, 0,sizeof(float)*location_size);
    memset(filter_indices, 0, sizeof(int)*num_priors);
    memset(props, 0, sizeof(float)*num_priors);

    int validCount = 0;
    const float VARIANCES[2] = {0.1, 0.2};

    int loc_zp =            ctx_->app_ctx.output_attrs[0].zp; 
    float loc_scale =       ctx_->app_ctx.output_attrs[0].scale;

    int scores_zp =         ctx_->app_ctx.output_attrs[1].zp;    
    float scores_scale =    ctx_->app_ctx.output_attrs[1].scale;

    int landms_zp =         ctx_->app_ctx.output_attrs[2].zp;    
    float landms_scale =    ctx_->app_ctx.output_attrs[2].scale;

    for(int i = 0;i < num_priors; i++)
    {
        float face_score = deqnt_affine_to_f32(scores[i*2+1], scores_zp, scores_scale);
        if (face_score > 0.5)
        {
            filter_indices[validCount] = i;
            props[validCount] = face_score; 
            int offset = i*4;
            uint8_t *bbox = location + offset;
            
            float box_x = ( deqnt_affine_to_f32(bbox[0],loc_zp,loc_scale)) * VARIANCES[0] * prior_ptr[i][2] + prior_ptr[i][0];
            float box_y = ( deqnt_affine_to_f32(bbox[1],loc_zp,loc_scale)) * VARIANCES[0] * prior_ptr[i][3] + prior_ptr[i][1];
            float box_w = (float) expf(( deqnt_affine_to_f32(bbox[2],loc_zp,loc_scale)) * VARIANCES[1]) * prior_ptr[i][2];
            float box_h = (float) expf(( deqnt_affine_to_f32(bbox[3],loc_zp,loc_scale)) * VARIANCES[1]) * prior_ptr[i][3];
    
            float xmin = box_x - box_w * 0.5f;
            float ymin = box_y - box_h * 0.5f;
            float xmax = xmin + box_w;
            float ymax = ymin + box_h;

            loc_fp32[offset + 0] = xmin;
            loc_fp32[offset + 1] = ymin;
            loc_fp32[offset + 2] = xmax;
            loc_fp32[offset + 3] = ymax;
            for(int j = 0; j < 5;j++)
            {
                landms_fp32[i * 10 + 2 * j] = deqnt_affine_to_f32(landms[i * 10 + 2 * j],landms_zp,landms_scale)
                                         * VARIANCES[0] * prior_ptr[i][2] + prior_ptr[i][0];
                landms_fp32[i * 10 + 2 * j + 1] = deqnt_affine_to_f32(landms[i * 10 + 2 * j + 1],landms_zp,landms_scale)
                                         * VARIANCES[0] * prior_ptr[i][3] + prior_ptr[i][1];
            }
      
            ++validCount;
        }
    }

    quick_sort_indice_inverse(props, 0, validCount - 1, filter_indices);

    nms(validCount, loc_fp32, filter_indices, 0.2, width, height);

    results.clear();
    int num_face_count = 0; 
    for (int i = 0; i < validCount; ++i) {
        if (num_face_count >= 128) {
            break;
        }
        if (filter_indices[i] == -1 || props[i] < 0.5) {
            continue;
        }

        int n = filter_indices[i];
        
        float x1 = loc_fp32[n * 4 + 0] * width;
        float y1 = loc_fp32[n * 4 + 1] * height;
        float x2 = loc_fp32[n * 4 + 2] * width;
        float y2 = loc_fp32[n * 4 + 3] * height;

        // Scale back to original image
        float scaleX = (float)img.cols / (float)width;
        float scaleY = (float)img.rows / (float)height;

        ObjectDet det;
        det.box = cv::Rect(
            (int)(clamp(x1, 0, width) * scaleX),
            (int)(clamp(y1, 0, height) * scaleY),
            (int)((clamp(x2, 0, width) - clamp(x1, 0, width)) * scaleX),
            (int)((clamp(y2, 0, height) - clamp(y1, 0, height)) * scaleY)
        );
        det.score = props[i];
        det.class_id = 0; // Face
        det.label = "face";

        for(int j = 0;j < 5;j++)
        {
            float point_x = landms_fp32[n * 10 + 2 * j] * width;
            float point_y = landms_fp32[n * 10 + 2 * j+1] * height;
            det.landmarks.push_back(cv::Point2f(point_x * scaleX, point_y * scaleY));
        }
        
        results.push_back(det);
        num_face_count++;
    }

    return 0;
}
