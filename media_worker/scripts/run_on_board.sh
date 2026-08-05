#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP_DIR=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
CONFIG_PATH=${1:-"${APP_DIR}/config/media_worker.example.json"}

export LD_LIBRARY_PATH="${APP_DIR}/lib:/oem/usr/lib:/oem/lib:${LD_LIBRARY_PATH:-}"

if pgrep -x aipc >/dev/null 2>&1; then
    echo "aipc is running and owns media hardware; stop it before media_worker" >&2
    exit 1
fi

if [ -x /oem/usr/bin/RkLunch-stop.sh ]; then
    /oem/usr/bin/RkLunch-stop.sh >/dev/null 2>&1 || true
fi

exec "${APP_DIR}/bin/media_worker" --config "${CONFIG_PATH}"

