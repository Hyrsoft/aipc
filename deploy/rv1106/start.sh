#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP_DIR=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
CONFIG_PATH=${AIPC_DAEMON_CONFIG:-"${APP_DIR}/config/aipc-daemon.json"}

mkdir -p "${APP_DIR}/data"
export LD_LIBRARY_PATH="${APP_DIR}/lib:/oem/usr/lib:/oem/lib:${LD_LIBRARY_PATH:-}"

rm -f /tmp/media_worker_video.h264 /tmp/media_worker_audio.g711a

# Migrate only the historical daemon defaults. Explicit recording paths chosen by
# the user remain untouched, while old installs stop filling the small /tmp tmpfs.
STATE_PATH="${APP_DIR}/data/state.json"
if [ -f "${STATE_PATH}" ]; then
    sed -i \
        -e 's#"/tmp/media_worker_video.h264"#"/dev/null"#g' \
        -e 's#"/tmp/media_worker_audio.g711a"#"/dev/null"#g' \
        "${STATE_PATH}"
fi

pkill -TERM media_worker >/dev/null 2>&1 || true
pkill -TERM aipc >/dev/null 2>&1 || true
if [ -x /oem/usr/bin/RkLunch-stop.sh ]; then
    /oem/usr/bin/RkLunch-stop.sh >/dev/null 2>&1 || true
else
    pkill -TERM rkipc >/dev/null 2>&1 || true
fi

if [ "${AIPC_LOG_TO_FILES:-0}" = "1" ]; then
    exec "${APP_DIR}/bin/aipc-daemon" --config "${CONFIG_PATH}" "$@" \
        >>"${APP_DIR}/data/daemon.stdout.log" 2>>"${APP_DIR}/data/daemon.stderr.log"
fi
exec "${APP_DIR}/bin/aipc-daemon" --config "${CONFIG_PATH}" "$@"
