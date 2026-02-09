#!/bin/bash

# 获取脚本所在目录的绝对路径
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd)
WWW_DIR="$SCRIPT_DIR/../www"

echo "========================================="
echo "构建前端项目 (Svelte + Tailwind)..."
echo "========================================="

if [ ! -d "$WWW_DIR" ]; then
    echo "错误：找不到 www 目录: $WWW_DIR"
    exit 1
fi

cd "$WWW_DIR"

if [ ! -d "node_modules" ]; then
    echo "正在安装前端依赖..."
    npm install
    if [ $? -ne 0 ]; then
        echo "错误：npm install 失败"
        exit 1
    fi
fi

echo "正在编译前端资源..."
npm run build
if [ $? -ne 0 ]; then
    echo "错误：npm run build 失败"
    exit 1
fi

echo "前端构建完成！输出目录: $WWW_DIR/dist"
echo "========================================="
