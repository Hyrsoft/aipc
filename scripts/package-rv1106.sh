#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET="armv7-unknown-linux-uclibceabihf"
PACKAGE_DIR="${AIPC_PACKAGE_DIR:-${PROJECT_ROOT}/target/package/aipc-rust}"
NATIVE_INSTALL="${PROJECT_ROOT}/target/native/RV1106Release/install"
RKNN_RUNTIME_DIR="${AIPC_RKNN_RUNTIME_DIR:-${PROJECT_ROOT}/target/rknn-runtime}"

if ! command -v jq >/dev/null 2>&1; then
    echo "jq is required to generate the daemon-managed worker config" >&2
    exit 1
fi

"${SCRIPT_DIR}/build-rv1106.sh"
"${SCRIPT_DIR}/fetch-ai-models.sh"
"${SCRIPT_DIR}/fetch-rknn-runtime.sh"
npm --prefix "${PROJECT_ROOT}/webui" run build

rm -rf "${PACKAGE_DIR}"
mkdir -p "${PACKAGE_DIR}/bin" "${PACKAGE_DIR}/config" "${PACKAGE_DIR}/data" \
    "${PACKAGE_DIR}/lib" "${PACKAGE_DIR}/scripts" "${PACKAGE_DIR}/www"
install -m 0755 "${PROJECT_ROOT}/target/${TARGET}/release/aipc-daemon" \
    "${PACKAGE_DIR}/bin/aipc-daemon"
install -m 0755 "${PROJECT_ROOT}/target/${TARGET}/release/media_worker" \
    "${PACKAGE_DIR}/bin/media_worker"
install -m 0755 "${PROJECT_ROOT}/target/${TARGET}/release/ai_worker" \
    "${PACKAGE_DIR}/bin/ai_worker"
install -m 0755 "${NATIVE_INSTALL}/lib/libvisiong.so" \
    "${PACKAGE_DIR}/lib/libvisiong.so"
install -m 0755 "${RKNN_RUNTIME_DIR}/librknnmrt.so" \
    "${PACKAGE_DIR}/lib/librknnmrt.so"
mkdir -p "${PACKAGE_DIR}/licenses/visiong" "${PACKAGE_DIR}/licenses/lua" \
    "${PACKAGE_DIR}/licenses/models" "${PACKAGE_DIR}/seed/ai/projects" "${PACKAGE_DIR}/seed/ai/models"
cp -a "${NATIVE_INSTALL}/licenses/visiong/." \
    "${PACKAGE_DIR}/licenses/visiong/"
install -m 0644 "${NATIVE_INSTALL}/licenses/lua/readme.html" \
    "${PACKAGE_DIR}/licenses/lua/readme.html"
install -m 0644 "${PROJECT_ROOT}/ai_worker/THIRD_PARTY.md" \
    "${PACKAGE_DIR}/licenses/AI_WORKER_THIRD_PARTY.md"
install -m 0644 "${PROJECT_ROOT}/ai_worker/MODEL_RELEASE.json" \
    "${PACKAGE_DIR}/licenses/models/manifest.json"
install -m 0644 "${PROJECT_ROOT}/ai_worker/MODEL_RELEASE_README.md" \
    "${PACKAGE_DIR}/licenses/models/README.md"
install -m 0644 "${PROJECT_ROOT}/ai_worker/MODEL_RELEASE_SHA256SUMS" \
    "${PACKAGE_DIR}/licenses/models/SHA256SUMS"
for source in "${PROJECT_ROOT}/ai_worker/examples"/*; do
    [ -d "${source}" ] || continue
    [ -f "${source}/manifest.json" ] || continue
    project_id="$(jq -r '.id' "${source}/manifest.json")"
    mkdir -p "${PACKAGE_DIR}/seed/ai/projects/${project_id}"
    install -m 0644 "${source}/manifest.json" \
        "${PACKAGE_DIR}/seed/ai/projects/${project_id}/manifest.json"
    install -m 0644 "${source}/main.lua" \
        "${PACKAGE_DIR}/seed/ai/projects/${project_id}/main.lua"
done
cp -a "${PROJECT_ROOT}/target/ai-models/." "${PACKAGE_DIR}/seed/ai/models/"
install -m 0644 "${PROJECT_ROOT}/config/aipc-daemon.example.json" \
    "${PACKAGE_DIR}/config/aipc-daemon.json"
jq '.video.output_path = "" | .audio.output_path = ""' \
    "${PROJECT_ROOT}/media_worker/config/media_worker.example.json" \
    >"${PACKAGE_DIR}/config/media_worker.json"
chmod 0644 "${PACKAGE_DIR}/config/media_worker.json"
jq '.data_dir = "/userdata/aipc-rust/data" | .recording.directory = "/userdata/aipc-rust/recordings" | .recording.allowed_roots = ["/userdata/aipc-rust/recordings"] | .dependencies.root = "/userdata/aipc-rust/data/dependencies"' \
    "${PACKAGE_DIR}/config/aipc-daemon.json" \
    >"${PACKAGE_DIR}/config/aipc-daemon.json.part"
mv "${PACKAGE_DIR}/config/aipc-daemon.json.part" "${PACKAGE_DIR}/config/aipc-daemon.json"
install -m 0755 "${PROJECT_ROOT}/deploy/rv1106/start.sh" "${PACKAGE_DIR}/scripts/start.sh"
install -m 0755 "${PROJECT_ROOT}/deploy/rv1106/launch.sh" "${PACKAGE_DIR}/scripts/launch.sh"
install -m 0755 "${PROJECT_ROOT}/deploy/rv1106/stop.sh" "${PACKAGE_DIR}/scripts/stop.sh"
install -m 0755 "${PROJECT_ROOT}/deploy/rv1106/init.sh" "${PACKAGE_DIR}/scripts/init.sh"
cp -a "${PROJECT_ROOT}/webui/dist/." "${PACKAGE_DIR}/www/"

echo "RV1106 package assembled at ${PACKAGE_DIR}"
