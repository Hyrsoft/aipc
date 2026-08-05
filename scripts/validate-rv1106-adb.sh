#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REMOTE_DIR="${AIPC_REMOTE_DIR:-/root/aipc-rust}"
LOCAL_PORT="${AIPC_ADB_HTTP_PORT:-18080}"
BASE_URL="http://127.0.0.1:${LOCAL_PORT}"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"; adb forward --remove "tcp:${LOCAL_PORT}" >/dev/null 2>&1 || true' EXIT

if [ "${AIPC_SKIP_DEPLOY:-0}" != "1" ]; then
    "${SCRIPT_DIR}/deploy-rv1106-adb.sh"
fi
adb forward "tcp:${LOCAL_PORT}" tcp:8080 >/dev/null

wait_running() {
    local expected_generation=${1:-}
    for _ in $(seq 1 90); do
        if curl -fsS "${BASE_URL}/api/v1/status" >"${WORK_DIR}/status.json"; then
            local state generation
            state=$(jq -r '.state' "${WORK_DIR}/status.json")
            generation=$(jq -r '.generation // ""' "${WORK_DIR}/status.json")
            if [ "${state}" = "running" ] && { [ -z "${expected_generation}" ] || [ "${generation}" = "${expected_generation}" ]; }; then
                return 0
            fi
        fi
        sleep 1
    done
    echo "worker did not reach Running" >&2
    cat "${WORK_DIR}/status.json" >&2 || true
    adb shell "tail -100 '${REMOTE_DIR}/data/daemon.stderr.log'" >&2 || true
    return 1
}

wait_running
curl -fsS "${BASE_URL}/healthz" | jq -e '.ok == true' >/dev/null
curl -fsS "${BASE_URL}/api/v1/config" >"${WORK_DIR}/state.json"
jq '.desired | .video.width=1280 | .video.height=720 | .video.fps=25 | .video.bitrate_kbps=2048 | .video.gop=25' \
    "${WORK_DIR}/state.json" >"${WORK_DIR}/720p.json"
generation=$(curl -fsS -X PUT -H 'content-type: application/json' --data-binary "@${WORK_DIR}/720p.json" \
    "${BASE_URL}/api/v1/config" | jq -r '.generation')
wait_running "${generation}"

jq '.video.width=1' "${WORK_DIR}/720p.json" >"${WORK_DIR}/invalid.json"
http_code=$(curl -sS -o "${WORK_DIR}/invalid-response.json" -w '%{http_code}' -X PUT \
    -H 'content-type: application/json' --data-binary "@${WORK_DIR}/invalid.json" "${BASE_URL}/api/v1/config")
[ "${http_code}" = "400" ]
[ "$(curl -fsS "${BASE_URL}/api/v1/status" | jq -r '.generation')" = "${generation}" ]

worker_pid=$(curl -fsS "${BASE_URL}/api/v1/status" | jq -r '.pid')
adb shell "kill -KILL '${worker_pid}'"
for _ in $(seq 1 45); do
    current=$(curl -fsS "${BASE_URL}/api/v1/status" || true)
    if [ "$(jq -r '.state // ""' <<<"${current}")" = "running" ] && \
       [ "$(jq -r '.generation // ""' <<<"${current}")" != "${generation}" ]; then
        generation=$(jq -r '.generation' <<<"${current}")
        break
    fi
    sleep 1
done

curl -fsS "${BASE_URL}/api/v1/config" | jq '.active | .isp.iq_dir="/missing/aipc-iqfiles"' >"${WORK_DIR}/rollback.json"
failed_generation=$(curl -fsS -X PUT -H 'content-type: application/json' --data-binary "@${WORK_DIR}/rollback.json" \
    "${BASE_URL}/api/v1/config" | jq -r '.generation')
for _ in $(seq 1 45); do
    current=$(curl -fsS "${BASE_URL}/api/v1/status" || true)
    if [ "$(jq -r '.state // ""' <<<"${current}")" = "running" ] && \
       [ "$(jq -r '.generation // ""' <<<"${current}")" != "${failed_generation}" ] && \
       [ "$(jq -r '.last_error // ""' <<<"${current}")" != "" ]; then
        break
    fi
    sleep 1
done

curl -fsS "${BASE_URL}/api/v1/config" | jq '.active' >"${WORK_DIR}/restore.json"
generation=$(curl -fsS -X PUT -H 'content-type: application/json' --data-binary "@${WORK_DIR}/restore.json" \
    "${BASE_URL}/api/v1/config" | jq -r '.generation')
wait_running "${generation}"

for _ in 1 2 3; do
    generation=$(curl -fsS -X POST "${BASE_URL}/api/v1/worker/restart" | jq -r '.generation')
    wait_running "${generation}"
done

curl -fsS "${BASE_URL}/" | grep -q 'AIPC Media Console'
adb shell "'${REMOTE_DIR}/scripts/stop.sh'"
if adb shell "pgrep -x media_worker" | grep -q '[0-9]'; then
    echo "media_worker remained after daemon shutdown" >&2
    exit 1
fi

echo "RV1106 daemon, worker, rollback, restart and Web UI validation passed"
