#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

RK_LIB_DIR="${PROJECT_ROOT}/3rdparty/luckfox_pico_rkmpi_example/lib/uclibc"
BUILD_DIR="${AIPC_BUILD_DIR:-build/Debug}"
DATA_LIB_DIR="${AIPC_LIBDATACHANNEL_DIR:-${PROJECT_ROOT}/${BUILD_DIR}/libdatachannel}"

REMOTE_HOST="${1:-${AIPC_REMOTE_HOST:-root@192.168.8.235}}"
REMOTE_DIR="${2:-${AIPC_REMOTE_LIB_DIR:-/usr/lib}}"
REMOTE_BIN_PATH="${3:-}"

copy_with_glob() {
    local pattern="$1"
    local matched=()

    while IFS= read -r file; do
        matched+=("$file")
    done < <(compgen -G "$pattern" || true)

    if [ ${#matched[@]} -eq 0 ]; then
        echo "跳过：未匹配到文件 -> $pattern"
        return 0
    fi

    echo "上传 ${#matched[@]} 个文件: $pattern"
    scp "${matched[@]}" "${REMOTE_HOST}:${REMOTE_DIR}/"
}

echo "========================================="
echo "上传动态库到开发板"
echo "REMOTE_HOST: ${REMOTE_HOST}"
echo "REMOTE_DIR : ${REMOTE_DIR}"
echo "========================================="

if [ ! -d "$RK_LIB_DIR" ]; then
    echo "错误：目录不存在 -> $RK_LIB_DIR"
    exit 1
fi

if [ ! -d "$DATA_LIB_DIR" ]; then
    echo "错误：目录不存在 -> $DATA_LIB_DIR"
    exit 1
fi

copy_with_glob "${RK_LIB_DIR}/*.so"
copy_with_glob "${RK_LIB_DIR}/*.so.*"
copy_with_glob "${DATA_LIB_DIR}/libdatachannel.so"
copy_with_glob "${DATA_LIB_DIR}/libdatachannel.so.*"

echo ""
echo "动态库上传完成。"

if [ -n "$REMOTE_BIN_PATH" ]; then
    echo ""
    echo "执行远端 ldd 检查: $REMOTE_BIN_PATH"
    ssh "$REMOTE_HOST" "ldd \"$REMOTE_BIN_PATH\""
fi
