#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PACKAGE_DIR="${AIPC_PACKAGE_DIR:-${PROJECT_ROOT}/target/package/aipc-rust}"
REMOTE_DIR="${AIPC_REMOTE_DIR:-/userdata/aipc-rust}"
REMOTE_DATA_DIR="${AIPC_REMOTE_DATA_DIR:-/userdata/aipc-rust/data}"
OLD_REMOTE_DIR="${AIPC_OLD_REMOTE_DIR:-/root/aipc-rust}"
OLD_DATA_DIR="${AIPC_OLD_DATA_DIR:-${OLD_REMOTE_DIR}/data}"
ADB=(adb)
if [ -n "${AIPC_ADB_SERIAL:-}" ]; then
    ADB+=(-s "${AIPC_ADB_SERIAL}")
fi

if [ "${AIPC_SKIP_BUILD:-0}" != "1" ]; then
    "${SCRIPT_DIR}/package-rv1106.sh"
fi

"${ADB[@]}" get-state >/dev/null
"${ADB[@]}" shell "rm -rf '${REMOTE_DIR}.new' && mkdir -p '${REMOTE_DIR}.new'"
"${ADB[@]}" push "${PACKAGE_DIR}/." "${REMOTE_DIR}.new/"
"${ADB[@]}" shell "if [ -x '${REMOTE_DIR}/scripts/stop.sh' ]; then '${REMOTE_DIR}/scripts/stop.sh' >/dev/null 2>&1 || true; fi; if [ -x '${OLD_REMOTE_DIR}/scripts/stop.sh' ] && [ '${OLD_REMOTE_DIR}' != '${REMOTE_DIR}' ]; then '${OLD_REMOTE_DIR}/scripts/stop.sh' >/dev/null 2>&1 || true; fi"
"${ADB[@]}" shell "rm -rf '${REMOTE_DIR}.previous' && if [ -d '${REMOTE_DIR}' ]; then mv '${REMOTE_DIR}' '${REMOTE_DIR}.previous'; fi && mv '${REMOTE_DIR}.new' '${REMOTE_DIR}'"
"${ADB[@]}" shell "mkdir -p '${REMOTE_DATA_DIR}' && if [ -d '${OLD_DATA_DIR}' ] && [ '${OLD_DATA_DIR}' != '${REMOTE_DATA_DIR}' ]; then cp -a '${OLD_DATA_DIR}/.' '${REMOTE_DATA_DIR}/'; fi && if [ -d '${REMOTE_DIR}.previous/data' ]; then cp -a '${REMOTE_DIR}.previous/data/.' '${REMOTE_DATA_DIR}/'; fi"
"${ADB[@]}" shell "mkdir -p '${REMOTE_DIR}/data' '${REMOTE_DIR}/lib'"
"${ADB[@]}" shell "ln -sfn '${REMOTE_DIR}/scripts/init.sh' /etc/init.d/S99aipc" || true
init_target=$("${ADB[@]}" shell "readlink /etc/init.d/S99aipc 2>/dev/null || true" | tr -d '\r')
if [ "${init_target}" != "${REMOTE_DIR}/scripts/init.sh" ]; then
    echo "warning: /etc/init.d is not writable; install the init link in the firmware or remount rootfs before reboot" >&2
fi
"${ADB[@]}" shell "'${REMOTE_DIR}/scripts/launch.sh'"

echo "Deployed and started AIPC daemon at ${REMOTE_DIR}"
