#!/bin/sh

set -eu

APP_DIR=/root/aipc-rust

case "${1:-}" in
    start)
        [ -x "${APP_DIR}/scripts/launch.sh" ] || exit 0
        if pgrep -x aipc-daemon >/dev/null 2>&1; then
            exit 0
        fi
        "${APP_DIR}/scripts/launch.sh"
        ;;
    stop)
        [ -x "${APP_DIR}/scripts/stop.sh" ] || exit 0
        "${APP_DIR}/scripts/stop.sh"
        ;;
    restart|reload)
        "$0" stop
        "$0" start
        ;;
    *)
        echo "Usage: $0 {start|stop|restart}"
        exit 1
        ;;
esac
