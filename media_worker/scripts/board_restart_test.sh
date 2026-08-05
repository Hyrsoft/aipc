#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP_DIR=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
CONFIG_PATH=${1:-"${APP_DIR}/config/media_worker.example.json"}
ROUNDS=${2:-3}
RUN_SECONDS=${3:-3}

export LD_LIBRARY_PATH="${APP_DIR}/lib:/oem/usr/lib:/oem/lib:${LD_LIBRARY_PATH:-}"

round=1
while [ "${round}" -le "${ROUNDS}" ]; do
    event_file="/tmp/media_worker_restart_${round}.jsonl"
    log_file="/tmp/media_worker_restart_${round}.log"
    rm -f "${event_file}" "${log_file}"

    "${APP_DIR}/bin/media_worker" --config "${CONFIG_PATH}" \
        --generation "restart-${round}" >"${event_file}" 2>"${log_file}" &
    worker_pid=$!
    sleep "${RUN_SECONDS}"
    kill -TERM "${worker_pid}"
    wait "${worker_pid}"

    if ! grep -q '"event":"Stopped"' "${event_file}"; then
        echo "round ${round}: missing Stopped event" >&2
        exit 1
    fi
    if ! grep -q '"exit_code":0' "${event_file}"; then
        echo "round ${round}: worker did not exit cleanly" >&2
        exit 1
    fi
    echo "round-${round}-ok"
    round=$((round + 1))
done

