#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP_DIR=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
PID_FILE="${APP_DIR}/data/daemon.pid"

mkdir -p "${APP_DIR}/data"
export AIPC_LOG_TO_FILES=1
if ! pgrep -x aipc-daemon >/dev/null 2>&1; then
    rm -f "${PID_FILE}"
fi
start-stop-daemon -S -b -m -p "${PID_FILE}" -x "${APP_DIR}/scripts/start.sh"
