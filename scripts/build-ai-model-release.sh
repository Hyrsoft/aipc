#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODEL_DIR="${AIPC_AI_MODEL_DIR:-${PROJECT_ROOT}/target/ai-models}"
OUTPUT_DIR="${AIPC_AI_MODEL_RELEASE_DIR:-${PROJECT_ROOT}/target/release-assets/ai-models-v1.0.0}"
ARCHIVE="${OUTPUT_DIR}/aipc-visiong-rv1106-models-1.0.0.tar.gz"
STAGING="${OUTPUT_DIR}/staging"

rm -rf "${STAGING}"
mkdir -p "${STAGING}/models" "${OUTPUT_DIR}"
cp "${PROJECT_ROOT}/ai_worker/MODEL_RELEASE.json" "${STAGING}/manifest.json"
cp "${PROJECT_ROOT}/ai_worker/MODEL_RELEASE_SHA256SUMS" "${STAGING}/SHA256SUMS"
cp "${PROJECT_ROOT}/ai_worker/MODEL_RELEASE_README.md" "${STAGING}/README.md"

while read -r sha path; do
    [ -n "${sha}" ] || continue
    name="${path#models/}"
    install -m 0644 "${MODEL_DIR}/${name}" "${STAGING}/models/${name}"
done <"${STAGING}/SHA256SUMS"

(cd "${STAGING}" && sha256sum --check SHA256SUMS)
tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
    -C "${STAGING}" -cf - README.md SHA256SUMS manifest.json models \
    | gzip -n >"${ARCHIVE}.part"
mv "${ARCHIVE}.part" "${ARCHIVE}"
(cd "${OUTPUT_DIR}" && sha256sum "$(basename "${ARCHIVE}")" >ARCHIVE_SHA256SUMS)
cp "${STAGING}/README.md" "${OUTPUT_DIR}/README.md"
cp "${STAGING}/manifest.json" "${OUTPUT_DIR}/manifest.json"
cp "${STAGING}/SHA256SUMS" "${OUTPUT_DIR}/SHA256SUMS"

echo "AI model release ready at ${OUTPUT_DIR}"
