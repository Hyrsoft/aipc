#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
RUNTIME_DIR="${AIPC_RKNN_RUNTIME_DIR:-${PROJECT_ROOT}/target/rknn-runtime}"
RUNTIME_PATH="${RUNTIME_DIR}/librknnmrt.so"
RKNN_COMMIT="59a913d172e7f5ff03c9076e2ec7b1b1288ffd08"
RKNN_URL="https://raw.githubusercontent.com/airockchip/rknn-toolkit2/${RKNN_COMMIT}/rknpu2/runtime/Linux/librknn_api/armhf-uclibc/librknnmrt.so"
RKNN_SHA256="5dcaf6201a019b24d99bb05560b79a1a53fdf81451b89e0ada541ccf31cd3e84"

mkdir -p "${RUNTIME_DIR}"
if [ ! -f "${RUNTIME_PATH}" ]; then
    curl -L --fail --output "${RUNTIME_PATH}.part" "${RKNN_URL}"
    mv "${RUNTIME_PATH}.part" "${RUNTIME_PATH}"
fi
echo "${RKNN_SHA256}  ${RUNTIME_PATH}" | sha256sum --check -

echo "RKNN runtime 2.3.2 ready at ${RUNTIME_PATH}"
