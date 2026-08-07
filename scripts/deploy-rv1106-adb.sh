#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PACKAGE_DIR="${AIPC_PACKAGE_DIR:-${PROJECT_ROOT}/target/package/aipc-rust}"
REMOTE_DIR="${AIPC_REMOTE_DIR:-/root/aipc-rust}"
ADB=(adb)
if [ -n "${AIPC_ADB_SERIAL:-}" ]; then
    ADB+=(-s "${AIPC_ADB_SERIAL}")
fi

if [ "${AIPC_SKIP_BUILD:-0}" != "1" ]; then
    "${SCRIPT_DIR}/package-rv1106.sh"
fi

"${ADB[@]}" get-state >/dev/null
"${ADB[@]}" shell "'${REMOTE_DIR}/scripts/stop.sh' >/dev/null 2>&1 || true"
"${ADB[@]}" shell "rm -rf '${REMOTE_DIR}.new' && mkdir -p '${REMOTE_DIR}.new'"
"${ADB[@]}" push "${PACKAGE_DIR}/." "${REMOTE_DIR}.new/"
"${ADB[@]}" shell "rm -rf '${REMOTE_DIR}.previous' && if [ -d '${REMOTE_DIR}' ]; then mv '${REMOTE_DIR}' '${REMOTE_DIR}.previous'; fi && mv '${REMOTE_DIR}.new' '${REMOTE_DIR}'"
"${ADB[@]}" shell "if [ -d '${REMOTE_DIR}.previous/data' ]; then cp -a '${REMOTE_DIR}.previous/data/.' '${REMOTE_DIR}/data/'; fi"
"${ADB[@]}" shell "mkdir -p '${REMOTE_DIR}/data' '${REMOTE_DIR}/lib'"
"${ADB[@]}" shell "'${REMOTE_DIR}/scripts/launch.sh'"

echo "Deployed and started AIPC daemon at ${REMOTE_DIR}"
