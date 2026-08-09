#!/bin/sh

set -eu

if [ -x /userdata/aipc-rust/scripts/launch.sh ]; then
    APP_DIR=/userdata/aipc-rust
else
    APP_DIR=/root/aipc-rust
fi
export AIPC_DATA_DIR=/userdata/aipc-rust/data

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
