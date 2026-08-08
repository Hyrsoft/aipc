#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODEL_DIR="${AIPC_AI_MODEL_DIR:-${PROJECT_ROOT}/target/ai-models}"
VISIONG_COMMIT="2c12bebe6852f522a61fa80a03bdefe3d40b2f17"
VISIONG_RAW="https://raw.githubusercontent.com/yiex/visiong/${VISIONG_COMMIT}/model"

mkdir -p "${MODEL_DIR}"

fetch() {
    local url="$1"
    local output="$2"
    local sha256="$3"
    if [ ! -f "${output}" ]; then
        curl -L --fail --output "${output}.part" "${url}"
        mv "${output}.part" "${output}"
    fi
    echo "${sha256}  ${output}" | sha256sum --check -
}

fetch "${VISIONG_RAW}/yolov5/coco80/yolov5n_coco80_640.rknn" "${MODEL_DIR}/yolov5n_coco80_640.rknn" "083b2cf8983a9956cb203b3cce1bb83e26690cc9429c7e07d2fd337b06fcccec"
fetch "${VISIONG_RAW}/yolov5/coco80/coco_80_labels_list.txt" "${MODEL_DIR}/coco_80_labels_list.txt" "d7654b26101572841ed1cd80aa03aa60e35f1b8acb4aea6906c4066886f16e07"
fetch "${VISIONG_RAW}/yolo11/coco80/yolo11n_coco80_640.rknn" "${MODEL_DIR}/yolo11n_coco80_640.rknn" "6e7b981aa28d8c161a7109ce9e75744b541793b932503f2e122123e71f2c3a4c"
fetch "${VISIONG_RAW}/yolov5/number/yolov5s_number_320.rknn" "${MODEL_DIR}/yolov5s_number_320.rknn" "1dd3c15ee304df76a5c3b93b076d95a56e3b3ce1ff7741cab2a399d8c1deb507"
fetch "${VISIONG_RAW}/yolov5/number/number.txt" "${MODEL_DIR}/number.txt" "fa39f85dc698e8c03824b0af3de7bc534da1cdf3905d1e8a585352854f5a7767"
fetch "${VISIONG_RAW}/lprnet/lprnet.rknn" "${MODEL_DIR}/lprnet.rknn" "b366132efd4121ecbd08d63bd01cc299ba11944d008646a38f7ff73b35e99d78"
fetch "${VISIONG_RAW}/mlsd/mlsd_320_large.rknn" "${MODEL_DIR}/mlsd_320_large.rknn" "c1eafadcd2b8295fc169e891c969e41ad6ad56e15c501152c7ce416b701cf7cb"
fetch "${VISIONG_RAW}/mlsd/mlsd_320_tiny.rknn" "${MODEL_DIR}/mlsd_320_tiny.rknn" "8282c137217f3889d007a45e486f0b380d6bf564ff6f2d7e6b5683182093d589"
fetch "${VISIONG_RAW}/nanotrack/T_model_backbone.rknn" "${MODEL_DIR}/T_model_backbone.rknn" "3a968b9fb8a61d1e4a98d2f8a3b3827a83b28596f1e8dc4b0e9d21f0624e64c8"
fetch "${VISIONG_RAW}/nanotrack/X_model_backbone.rknn" "${MODEL_DIR}/X_model_backbone.rknn" "802a1577e1613355b28d8b65ef5fae5d44bd41ab77ba9195fe004b6122defa2e"
fetch "${VISIONG_RAW}/nanotrack/model_head.rknn" "${MODEL_DIR}/model_head.rknn" "64c9c9731da278e66ee9a4bb3fed261815ba3ace0db694c54ed2ef8968a191c2"
fetch "${VISIONG_RAW}/ppocr/ppocrv3_det.rknn" "${MODEL_DIR}/ppocrv3_det.rknn" "9532a5289652f8e5c4ec5d2e849c8d3dca198b370edb359a0e0f61ed1710f6fe"
fetch "${VISIONG_RAW}/ppocr/ppocrv4_rec.rknn" "${MODEL_DIR}/ppocrv4_rec.rknn" "935e5e7db324ce04c27214b4c3c63d27ce1fd663afaa1d056e0847315a34d22d"
fetch "${VISIONG_RAW}/ppocr/ppocr_keys_v1.txt" "${MODEL_DIR}/ppocr_keys_v1.txt" "28b2362ad4ab2dc38769aa72feb535e3a9ddb3fd2a7585a05920e6393b1dc7f7"
fetch "${VISIONG_RAW}/ppocr/ppocrv6_rec.rknn" "${MODEL_DIR}/ppocrv6_rec.rknn" "45df66cca247cc171ea199ddf36a26a8fd076fd29c8158fd89586d202d473d72"
fetch "${VISIONG_RAW}/ppocr/ppocr_keys_v6.txt" "${MODEL_DIR}/ppocr_keys_v6.txt" "c5cbe34ef40c29c4df07ed012bf96569cb69a2d2a01a07027e9f13cb832bd9cd"

# The eight-class YOLO11 number model is distributed with the user's sample
# archive but is not published in the pinned VisionG model tree.  Import it
# when the archive is available; keep the project visible otherwise so the
# daemon can report the missing resource instead of silently dropping it.
SAMPLE_ARCHIVE="${AIPC_VISIONG_SAMPLE_ARCHIVE:-}"
NUMBER_YOLO11="${MODEL_DIR}/yolo11n_number_320.rknn"
NUMBER_YOLO11_SHA="38ab21657066b31c3f1e777ee37f24fc4be5aa62dfd654b61f3f0e73d5b1afef"
if [ -n "${SAMPLE_ARCHIVE}" ] && [ -f "${SAMPLE_ARCHIVE}" ]; then
    unzip -p "${SAMPLE_ARCHIVE}" "yolo11n_number_320.rknn" >"${NUMBER_YOLO11}.part"
    mv "${NUMBER_YOLO11}.part" "${NUMBER_YOLO11}"
elif [ ! -f "${NUMBER_YOLO11}" ]; then
    echo "note: yolo11n_number_320.rknn is unavailable; set AIPC_VISIONG_SAMPLE_ARCHIVE to import it" >&2
fi
if [ -f "${NUMBER_YOLO11}" ]; then
    echo "${NUMBER_YOLO11_SHA}  ${NUMBER_YOLO11}" | sha256sum --check -
fi

NCC_TEMPLATE="${PROJECT_ROOT}/ai_worker/example-assets/ncc_template.jpg"
echo "2bb78629be3a6b29025d85db3677642c267d0ff176706ab963e8cb0c132df10b  ${NCC_TEMPLATE}" | sha256sum --check -
install -m 0644 "${NCC_TEMPLATE}" "${MODEL_DIR}/ncc_template.jpg"

echo "AI example resources ready at ${MODEL_DIR}"
