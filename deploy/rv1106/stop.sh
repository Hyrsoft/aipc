#!/bin/sh

set -eu

pkill -TERM aipc-daemon >/dev/null 2>&1 || true
for _ in 1 2 3 4 5 6; do
    if ! pgrep -x aipc-daemon >/dev/null 2>&1; then
        exit 0
    fi
    sleep 1
done
pkill -KILL aipc-daemon >/dev/null 2>&1 || true
pkill -TERM ai_worker >/dev/null 2>&1 || true
pkill -TERM media_worker >/dev/null 2>&1 || true
