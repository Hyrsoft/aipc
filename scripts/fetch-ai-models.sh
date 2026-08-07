#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODEL_DIR="${AIPC_AI_MODEL_DIR:-${PROJECT_ROOT}/target/ai-models}"
VISIONG_COMMIT="2c12bebe6852f522a61fa80a03bdefe3d40b2f17"
MODEL_URL="https://raw.githubusercontent.com/yiex/visiong/${VISIONG_COMMIT}/model/yolov5/coco80/yolov5n_coco80_640.rknn"
LABELS_URL="https://raw.githubusercontent.com/yiex/visiong/${VISIONG_COMMIT}/model/yolov5/coco80/coco_80_labels_list.txt"
MODEL_SHA256="083b2cf8983a9956cb203b3cce1bb83e26690cc9429c7e07d2fd337b06fcccec"
LABELS_SHA256="d7654b26101572841ed1cd80aa03aa60e35f1b8acb4aea6906c4066886f16e07"

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

fetch "${MODEL_URL}" "${MODEL_DIR}/yolov5n_coco80_640.rknn" "${MODEL_SHA256}"
fetch "${LABELS_URL}" "${MODEL_DIR}/coco_80_labels_list.txt" "${LABELS_SHA256}"
echo "AI sample model ready at ${MODEL_DIR}"
