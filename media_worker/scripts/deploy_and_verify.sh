#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_PRESET="${MEDIA_WORKER_BUILD_PRESET:-Release}"
BUILD_DIR="${PROJECT_DIR}/build/${BUILD_PRESET}"
INSTALL_DIR="${BUILD_DIR}/install"
REMOTE_DIR="${MEDIA_WORKER_REMOTE_DIR:-/root/media_worker}"
VERIFY_DIR="${MEDIA_WORKER_VERIFY_DIR:-${BUILD_DIR}/verify}"
DURATION_SEC="${MEDIA_WORKER_VERIFY_DURATION_SEC:-10}"

cmake --preset "${BUILD_PRESET}" -S "${PROJECT_DIR}"
cmake --build --preset "${BUILD_PRESET}"
cmake --install "${BUILD_DIR}"

mkdir -p "${VERIFY_DIR}"
adb shell "mkdir -p '${REMOTE_DIR}/bin' '${REMOTE_DIR}/config'"
adb push "${INSTALL_DIR}/bin/media_worker" "${REMOTE_DIR}/bin/media_worker"
adb push "${INSTALL_DIR}/bin/run_on_board.sh" "${REMOTE_DIR}/bin/run_on_board.sh"
adb push "${INSTALL_DIR}/bin/board_restart_test.sh" \
    "${REMOTE_DIR}/bin/board_restart_test.sh"
adb push "${INSTALL_DIR}/bin/board_negative_test.sh" \
    "${REMOTE_DIR}/bin/board_negative_test.sh"
adb push "${INSTALL_DIR}/config/media_worker.example.json" \
    "${REMOTE_DIR}/config/media_worker.example.json"

adb shell "pkill -TERM media_worker 2>/dev/null || true"
adb shell "/oem/usr/bin/RkLunch-stop.sh >/dev/null 2>&1 || true"
adb shell "rm -f /tmp/media_worker_video.h264 /tmp/media_worker_audio.g711a \
    /tmp/media_worker_events.jsonl /tmp/media_worker_stderr.log"

set +e
adb shell "LD_LIBRARY_PATH='${REMOTE_DIR}/lib:/oem/usr/lib:/oem/lib' \
    '${REMOTE_DIR}/bin/media_worker' \
    --config '${REMOTE_DIR}/config/media_worker.example.json' \
    --generation 'adb-verify' --duration-sec '${DURATION_SEC}' \
    --video-output /tmp/media_worker_video.h264 \
    --audio-output /tmp/media_worker_audio.g711a \
    >/tmp/media_worker_events.jsonl 2>/tmp/media_worker_stderr.log"
REMOTE_STATUS=$?
set -e

adb pull /tmp/media_worker_video.h264 "${VERIFY_DIR}/video.h264"
adb pull /tmp/media_worker_audio.g711a "${VERIFY_DIR}/audio.g711a"
adb pull /tmp/media_worker_events.jsonl "${VERIFY_DIR}/events.jsonl"
adb pull /tmp/media_worker_stderr.log "${VERIFY_DIR}/stderr.log"

if [ "${REMOTE_STATUS}" -ne 0 ]; then
    echo "media_worker exited with status ${REMOTE_STATUS}" >&2
    sed -n '1,200p' "${VERIFY_DIR}/events.jsonl" >&2
    sed -n '1,200p' "${VERIFY_DIR}/stderr.log" >&2
    exit "${REMOTE_STATUS}"
fi

grep -q '"event":"StreamReady".*"media":"video"' "${VERIFY_DIR}/events.jsonl"
grep -q '"event":"StreamReady".*"media":"audio"' "${VERIFY_DIR}/events.jsonl"
grep -q '"event":"Stopped"' "${VERIFY_DIR}/events.jsonl"

ffprobe -v error -f h264 -show_entries stream=codec_name,width,height \
    -of default=noprint_wrappers=1 "${VERIFY_DIR}/video.h264"
ffprobe -v error -f alaw -ar 8000 -show_entries stream=codec_name,sample_rate,channels \
    -of default=noprint_wrappers=1 "${VERIFY_DIR}/audio.g711a"

adb shell "'${REMOTE_DIR}/bin/board_restart_test.sh' \
    '${REMOTE_DIR}/config/media_worker.example.json' 3 3"
adb shell "'${REMOTE_DIR}/bin/board_negative_test.sh' \
    '${REMOTE_DIR}/config/media_worker.example.json'"

echo "media_worker verification artifacts: ${VERIFY_DIR}"
