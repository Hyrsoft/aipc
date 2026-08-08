#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODEL_DIR="${AIPC_AI_MODEL_DIR:-${PROJECT_ROOT}/target/ai-models}"
CACHE_DIR="${AIPC_AI_MODEL_CACHE_DIR:-${PROJECT_ROOT}/target/cache/ai-models}"
RELEASE_TAG="ai-models-v1.0.0"
ARCHIVE_NAME="aipc-visiong-rv1106-models-1.0.0.tar.gz"
ARCHIVE_SHA256="25af5bd4b7fceeeb791628fd651cbcaa27cd1f7e93367cbb7896e76bb074fd46"
RELEASE_URL="https://github.com/haoyn231/aipc/releases/download/${RELEASE_TAG}/${ARCHIVE_NAME}"
LOCAL_ARCHIVE="${AIPC_AI_MODELS_ARCHIVE:-}"

mkdir -p "${MODEL_DIR}" "${CACHE_DIR}"
if [ -n "${LOCAL_ARCHIVE}" ]; then
    ARCHIVE_PATH="${LOCAL_ARCHIVE}"
else
    ARCHIVE_PATH="${CACHE_DIR}/${ARCHIVE_NAME}"
    if [ ! -f "${ARCHIVE_PATH}" ]; then
        curl -L --fail --output "${ARCHIVE_PATH}.part" "${RELEASE_URL}"
        mv "${ARCHIVE_PATH}.part" "${ARCHIVE_PATH}"
    fi
fi

echo "${ARCHIVE_SHA256}  ${ARCHIVE_PATH}" | sha256sum --check -
STAGING="$(mktemp -d "${CACHE_DIR}/extract.XXXXXX")"
trap 'rm -rf "${STAGING}"' EXIT
tar -xzf "${ARCHIVE_PATH}" -C "${STAGING}"
(cd "${STAGING}" && sha256sum --check SHA256SUMS)
jq -e '.schema_version == 1 and .release == "ai-models-v1.0.0" and (.resources | length == 18)' \
    "${STAGING}/manifest.json" >/dev/null

while read -r sha path; do
    [ -n "${sha}" ] || continue
    name="${path#models/}"
    install -m 0644 "${STAGING}/${path}" "${MODEL_DIR}/.${name}.part"
    mv "${MODEL_DIR}/.${name}.part" "${MODEL_DIR}/${name}"
done <"${STAGING}/SHA256SUMS"

echo "AI example resources ready from ${RELEASE_TAG} at ${MODEL_DIR}"
