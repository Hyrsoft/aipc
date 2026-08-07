#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
AIPC_SDK_ROOT="${AIPC_SDK_ROOT:-$(cd "${PROJECT_ROOT}/.." && pwd)}"
TARGET="armv7-unknown-linux-uclibceabihf"
CROSS_PREFIX="arm-rockchip830-linux-uclibcgnueabihf"
TOOLCHAIN_DIR="${AIPC_SDK_ROOT}/tools/linux/toolchain/${CROSS_PREFIX}"
CROSS_GCC="${TOOLCHAIN_DIR}/bin/${CROSS_PREFIX}-gcc"
CROSS_GXX="${TOOLCHAIN_DIR}/bin/${CROSS_PREFIX}-g++"
CROSS_AR="${TOOLCHAIN_DIR}/bin/${CROSS_PREFIX}-ar"
OPENSSL_DIR="${AIPC_OPENSSL_DIR:-${AIPC_SDK_ROOT}/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot/usr}"

for tool in "${CROSS_GCC}" "${CROSS_GXX}" "${CROSS_AR}"; do
    if [ ! -x "${tool}" ]; then
        echo "missing RV1106 toolchain executable: ${tool}" >&2
        exit 1
    fi
done

"${SCRIPT_DIR}/fetch-ai-deps.sh"

if [ ! -f "${OPENSSL_DIR}/include/openssl/ssl.h" ] || [ ! -e "${OPENSSL_DIR}/lib/libssl.so" ]; then
    echo "missing target OpenSSL 1.1.1 sysroot at ${OPENSSL_DIR}; set AIPC_OPENSSL_DIR" >&2
    exit 1
fi

rustup component add rust-src --toolchain nightly >/dev/null

export AIPC_SDK_ROOT
export CC_armv7_unknown_linux_uclibceabihf="${CROSS_GCC}"
export CXX_armv7_unknown_linux_uclibceabihf="${CROSS_GXX}"
export AR_armv7_unknown_linux_uclibceabihf="${CROSS_AR}"
export CARGO_TARGET_ARMV7_UNKNOWN_LINUX_UCLIBCEABIHF_LINKER="${CROSS_GCC}"
export CFLAGS_armv7_unknown_linux_uclibceabihf="-fPIC"
export CXXFLAGS_armv7_unknown_linux_uclibceabihf="-fPIC"
export RUSTFLAGS="-C link-arg=-Wl,--gc-sections ${EXTRA_RUSTFLAGS:-}"
export OPENSSL_DIR
export OPENSSL_NO_VENDOR=1

cd "${PROJECT_ROOT}"
cargo +nightly build \
    -Z build-std=std,panic_abort \
    --target "${TARGET}" \
    --release \
    --package aipc-daemon

file "target/${TARGET}/release/aipc-daemon"
file "target/${TARGET}/release/media_worker"
