#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP_DIR=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
CONFIG_PATH=${1:-"${APP_DIR}/config/media_worker.example.json"}
export LD_LIBRARY_PATH="${APP_DIR}/lib:/oem/usr/lib:/oem/lib:${LD_LIBRARY_PATH:-}"

if "${APP_DIR}/bin/media_worker" --config "${CONFIG_PATH}" --fps 0 --no-audio \
    >/tmp/media_worker_invalid.jsonl 2>/tmp/media_worker_invalid.log; then
    echo "invalid configuration unexpectedly succeeded" >&2
    exit 1
else
    status=$?
fi
if [ "${status}" -ne 2 ]; then
    echo "invalid configuration returned ${status}, expected 2" >&2
    exit 1
fi
grep -q '"event":"FatalError"' /tmp/media_worker_invalid.jsonl

if "${APP_DIR}/bin/media_worker" --config "${CONFIG_PATH}" --no-audio \
    --video-output /proc/media_worker.h264 \
    >/tmp/media_worker_unwritable.jsonl 2>/tmp/media_worker_unwritable.log; then
    echo "unwritable output unexpectedly succeeded" >&2
    exit 1
else
    status=$?
fi
if [ "${status}" -ne 3 ]; then
    echo "unwritable output returned ${status}, expected 3" >&2
    exit 1
fi
grep -q '"event":"FatalError"' /tmp/media_worker_unwritable.jsonl
grep -q '"event":"Stopped"' /tmp/media_worker_unwritable.jsonl

echo "negative-tests-ok"

