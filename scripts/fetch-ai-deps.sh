#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEPS_DIR="${AIPC_AI_DEPS_DIR:-${PROJECT_ROOT}/target/ai-deps}"
CACHE_DIR="${AIPC_DOWNLOAD_CACHE:-${PROJECT_ROOT}/target/downloads}"

VISIONG_URL="https://github.com/yiex/visiong/releases/download/v1.2.1/visiong_cpp.zip"
VISIONG_SHA256="56336cc25150692e21505626b9f359b5dfeaa019f240460c2541b0bfdbe51bc0"
LUA_URL="https://www.lua.org/ftp/lua-5.4.8.tar.gz"
LUA_SHA256="4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae"

mkdir -p "${DEPS_DIR}" "${CACHE_DIR}"

fetch() {
    local url="$1"
    local output="$2"
    local sha256="$3"
    if [ ! -f "${output}" ]; then
        curl -L --fail --output "${output}.part" "${url}"
        mv "${output}.part" "${output}"
    fi
    echo "${sha256}  ${output}" | sha256sum --check -
}

fetch "${VISIONG_URL}" "${CACHE_DIR}/visiong_cpp-v1.2.1.zip" "${VISIONG_SHA256}"
fetch "${LUA_URL}" "${CACHE_DIR}/lua-5.4.8.tar.gz" "${LUA_SHA256}"

if [ ! -f "${DEPS_DIR}/visiong/.ready-v1.2.1" ]; then
    rm -rf "${DEPS_DIR}/visiong"
    mkdir -p "${DEPS_DIR}/visiong"
    unzip -q "${CACHE_DIR}/visiong_cpp-v1.2.1.zip" -d "${DEPS_DIR}/visiong"
    touch "${DEPS_DIR}/visiong/.ready-v1.2.1"
fi

if [ ! -f "${DEPS_DIR}/lua/.ready-5.4.8" ]; then
    rm -rf "${DEPS_DIR}/lua"
    mkdir -p "${DEPS_DIR}/lua"
    tar -xzf "${CACHE_DIR}/lua-5.4.8.tar.gz" --strip-components=1 -C "${DEPS_DIR}/lua"
    touch "${DEPS_DIR}/lua/.ready-5.4.8"
fi

echo "AI dependencies ready at ${DEPS_DIR}"
