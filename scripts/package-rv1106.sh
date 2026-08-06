#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET="armv7-unknown-linux-uclibceabihf"
PACKAGE_DIR="${AIPC_PACKAGE_DIR:-${PROJECT_ROOT}/target/package/aipc-rust}"

if ! command -v jq >/dev/null 2>&1; then
    echo "jq is required to generate the daemon-managed worker config" >&2
    exit 1
fi

"${SCRIPT_DIR}/build-rv1106.sh"
npm --prefix "${PROJECT_ROOT}/webui" run build

rm -rf "${PACKAGE_DIR}"
mkdir -p "${PACKAGE_DIR}/bin" "${PACKAGE_DIR}/config" "${PACKAGE_DIR}/data" \
    "${PACKAGE_DIR}/lib" "${PACKAGE_DIR}/scripts" "${PACKAGE_DIR}/www"
install -m 0755 "${PROJECT_ROOT}/target/${TARGET}/release/aipc-daemon" \
    "${PACKAGE_DIR}/bin/aipc-daemon"
install -m 0755 "${PROJECT_ROOT}/target/${TARGET}/release/media_worker" \
    "${PACKAGE_DIR}/bin/media_worker"
install -m 0644 "${PROJECT_ROOT}/config/aipc-daemon.example.json" \
    "${PACKAGE_DIR}/config/aipc-daemon.json"
jq '.video.output_path = "" | .audio.output_path = ""' \
    "${PROJECT_ROOT}/media_worker/config/media_worker.example.json" \
    >"${PACKAGE_DIR}/config/media_worker.json"
chmod 0644 "${PACKAGE_DIR}/config/media_worker.json"
install -m 0755 "${PROJECT_ROOT}/deploy/rv1106/start.sh" "${PACKAGE_DIR}/scripts/start.sh"
install -m 0755 "${PROJECT_ROOT}/deploy/rv1106/launch.sh" "${PACKAGE_DIR}/scripts/launch.sh"
install -m 0755 "${PROJECT_ROOT}/deploy/rv1106/stop.sh" "${PACKAGE_DIR}/scripts/stop.sh"
cp -a "${PROJECT_ROOT}/webui/dist/." "${PACKAGE_DIR}/www/"

echo "RV1106 package assembled at ${PACKAGE_DIR}"
