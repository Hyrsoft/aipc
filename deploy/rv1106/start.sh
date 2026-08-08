#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP_DIR=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
CONFIG_PATH=${AIPC_DAEMON_CONFIG:-"${APP_DIR}/config/aipc-daemon.json"}
if [ -z "${AIPC_DATA_DIR:-}" ]; then
    if [ -d /userdata ] && [ -w /userdata ]; then
        AIPC_DATA_DIR=/userdata/aipc-rust/data
    else
        AIPC_DATA_DIR="${APP_DIR}/data"
    fi
fi
export AIPC_DATA_DIR

mkdir -p "${AIPC_DATA_DIR}"
mkdir -p "${AIPC_DATA_DIR}/dependencies/active"
export LD_LIBRARY_PATH="${AIPC_DATA_DIR}/dependencies/active:${APP_DIR}/lib:/oem/usr/lib:/oem/lib:${LD_LIBRARY_PATH:-}"

mkdir -p "${AIPC_DATA_DIR}/ai/projects" "${AIPC_DATA_DIR}/ai/models"
if [ -d "${APP_DIR}/seed/ai/projects" ]; then
    for project in "${APP_DIR}"/seed/ai/projects/*; do
        [ -d "${project}" ] || continue
        target="${AIPC_DATA_DIR}/ai/projects/$(basename "${project}")"
        [ -e "${target}" ] || cp -a "${project}" "${target}"
    done
fi
if [ -d "${APP_DIR}/seed/ai/models" ]; then
    for model in "${APP_DIR}"/seed/ai/models/*; do
        [ -f "${model}" ] || continue
        target="${AIPC_DATA_DIR}/ai/models/$(basename "${model}")"
        [ -e "${target}" ] || cp -a "${model}" "${target}"
    done
fi

rm -f /tmp/media_worker_video.h264 /tmp/media_worker_audio.g711a

# Migrate only the historical daemon defaults. Explicit recording paths chosen by
# the user remain untouched, while old installs stop filling the small /tmp tmpfs.
STATE_PATH="${AIPC_DATA_DIR}/state.json"
if [ -f "${STATE_PATH}" ]; then
    sed -i \
        -e 's#"/tmp/media_worker_video.h264"#"/dev/null"#g' \
        -e 's#"/tmp/media_worker_audio.g711a"#"/dev/null"#g' \
        "${STATE_PATH}"
fi

pkill -TERM ai_worker >/dev/null 2>&1 || true
pkill -TERM media_worker >/dev/null 2>&1 || true
if [ -x /oem/usr/bin/RkLunch-stop.sh ]; then
    /oem/usr/bin/RkLunch-stop.sh >/dev/null 2>&1 || true
else
    pkill -TERM rkipc >/dev/null 2>&1 || true
fi

if [ "${AIPC_LOG_TO_FILES:-0}" = "1" ]; then
    exec "${APP_DIR}/bin/aipc-daemon" --config "${CONFIG_PATH}" "$@" \
        >>"${AIPC_DATA_DIR}/daemon.stdout.log" 2>>"${AIPC_DATA_DIR}/daemon.stderr.log"
fi
exec "${APP_DIR}/bin/aipc-daemon" --config "${CONFIG_PATH}" "$@"
