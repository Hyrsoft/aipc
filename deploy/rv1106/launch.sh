#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP_DIR=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
if [ -z "${AIPC_DATA_DIR:-}" ]; then
    if [ -d /userdata ] && [ -w /userdata ]; then
        AIPC_DATA_DIR=/userdata/aipc-rust/data
    else
        AIPC_DATA_DIR="${APP_DIR}/data"
    fi
fi
export AIPC_DATA_DIR
PID_FILE="${AIPC_DATA_DIR}/daemon.pid"

mkdir -p "${AIPC_DATA_DIR}"
export AIPC_LOG_TO_FILES=1
if ! pgrep -x aipc-daemon >/dev/null 2>&1; then
    rm -f "${PID_FILE}"
fi
start-stop-daemon -S -b -m -p "${PID_FILE}" -x "${APP_DIR}/scripts/start.sh"
