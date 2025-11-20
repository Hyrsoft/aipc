#include "YoloV5Engine.hpp"
#include "src/utils/postprocess.h"
#include <iostream>
#include <opencv2/imgproc.hpp>

// Helper functions from yolov5.cpp
static void dump_tensor_attr(rknn_tensor_attr *attr)
{
    printf("  index=%d, name=%s, n_dims=%d, dims=[%d, %d, %d, %d], n_elems=%d, size=%d, fmt=%s, type=%s, qnt_type=%s, "
           "zp=%d, scale=%f\n",
           attr->index, attr->name, attr->n_dims, attr->dims[0], attr->dims[1], attr->dims[2], attr->dims[3],
           attr->n_elems, attr->size, get_format_string(attr->fmt), get_type_string(attr->type),
           get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

struct YoloV5Context {
    rknn_app_context_t app_ctx;
    int model_width = 640;
    int model_height = 640;
    float scale;
    int leftPadding;
    int topPadding;
};

YoloV5Engine::YoloV5Engine() : ctx_(std::make_unique<YoloV5Context>()) {
    memset(&ctx_->app_ctx, 0, sizeof(rknn_app_context_t));
}

YoloV5Engine::~YoloV5Engine() {
    if (ctx_->app_ctx.rknn_ctx != 0) {
        // Release memory
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
        rknn_destroy(ctx_->app_ctx.rknn_ctx);
        deinit_post_process();
    }
}

int YoloV5Engine::Init(const std::string& model_path) {
    int ret;
    rknn_context ctx = 0;

    ret = rknn_init(&ctx, (char *)model_path.c_str(), 0, 0, NULL);
    if (ret < 0) {
        printf("rknn_init fail! ret=%d\n", ret);
        return -1;
    }

    // Get Model Input Output Number
    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        printf("rknn_query fail! ret=%d\n", ret);
        return -1;
    }

    // Get Model Input Info
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

    // Get Model Output Info
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

    // default input type is int8 (normalize and quantize need compute in outside)
    // if set uint8, will fuse normalize and quantize to npu
    input_attrs[0].type = RKNN_TENSOR_UINT8;
    input_attrs[0].fmt = RKNN_TENSOR_NHWC;
    ctx_->app_ctx.input_mems[0] = rknn_create_mem(ctx, input_attrs[0].size_with_stride);

    // Set input tensor memory
    ret = rknn_set_io_mem(ctx, ctx_->app_ctx.input_mems[0], &input_attrs[0]);
    if (ret < 0) {
        printf("input_mems rknn_set_io_mem fail! ret=%d\n", ret);
        return -1;
    }

    // Set output tensor memory
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
    
    ctx_->model_width = ctx_->app_ctx.model_width;
    ctx_->model_height = ctx_->app_ctx.model_height;

    init_post_process();
    return 0;
}

int YoloV5Engine::Inference(const cv::Mat& img, std::vector<ObjectDet>& results) {
    if (ctx_->app_ctx.rknn_ctx == 0) return -1;

    // Letterbox
    float scaleX = (float)ctx_->model_width / (float)img.cols;
    float scaleY = (float)ctx_->model_height / (float)img.rows;
    ctx_->scale = scaleX < scaleY ? scaleX : scaleY;
    
    int inputWidth = (int)((float)img.cols * ctx_->scale);
    int inputHeight = (int)((float)img.rows * ctx_->scale);
    
    ctx_->leftPadding = (ctx_->model_width - inputWidth) / 2;
    ctx_->topPadding = (ctx_->model_height - inputHeight) / 2;
    
    cv::Mat inputScale;
    cv::resize(img, inputScale, cv::Size(inputWidth, inputHeight), 0, 0, cv::INTER_LINEAR);
    cv::Mat letterboxImage(ctx_->model_height, ctx_->model_width, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Rect roi(ctx_->leftPadding, ctx_->topPadding, inputWidth, inputHeight);
    inputScale.copyTo(letterboxImage(roi));

    // Copy to input memory
    memcpy(ctx_->app_ctx.input_mems[0]->virt_addr, letterboxImage.data, ctx_->model_width * ctx_->model_height * 3);

    // Run inference
    int ret = rknn_run(ctx_->app_ctx.rknn_ctx, nullptr);
    if (ret < 0) {
        printf("rknn_run fail! ret=%d\n", ret);
        return -1;
    }

    // Post process
    object_detect_result_list od_results;
    // Use default thresholds
    post_process(&ctx_->app_ctx, ctx_->app_ctx.output_mems, BOX_THRESH, NMS_THRESH, &od_results);

    // Convert results
    results.clear();
    for (int i = 0; i < od_results.count; i++) {
        object_detect_result *det_result = &(od_results.results[i]);
        
        int x1 = det_result->box.left;
        int y1 = det_result->box.top;
        int x2 = det_result->box.right;
        int y2 = det_result->box.bottom;

        // Map coordinates back to original image
        int mx1 = x1 - ctx_->leftPadding;
        int my1 = y1 - ctx_->topPadding;
        int mx2 = x2 - ctx_->leftPadding;
        int my2 = y2 - ctx_->topPadding;

        x1 = (int)((float)mx1 / ctx_->scale);
        y1 = (int)((float)my1 / ctx_->scale);
        x2 = (int)((float)mx2 / ctx_->scale);
        y2 = (int)((float)my2 / ctx_->scale);

        ObjectDet det;
        det.box = cv::Rect(x1, y1, x2 - x1, y2 - y1);
        det.score = det_result->prop;
        det.class_id = det_result->cls_id;
        det.label = coco_cls_to_name(det_result->cls_id);
        results.push_back(det);
    }

    return 0;
}
