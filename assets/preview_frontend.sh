#!/bin/bash

# 获取脚本所在目录的绝对路径
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd)
WWW_DIR="$SCRIPT_DIR/../www"

echo "========================================="
echo "启动前端本地预览 (Web Mock Mode)..."
echo "========================================="
echo "在开发模式下，前端将使用模拟数据，不连接真实硬件。"
echo "如果需要连接真实设备，请修改 www/src/App.svelte 中的配置。"
echo ""

if [ ! -d "$WWW_DIR" ]; then
    echo "错误：找不到 www 目录: $WWW_DIR"
    exit 1
fi

cd "$WWW_DIR"

# 检查 npm 是否安装
if ! command -v npm &> /dev/null; then
    echo "错误：未找到 npm。请先安装 Node.js 和 npm。"
    exit 1
fi

if [ ! -d "node_modules" ]; then
    echo "正在安装依赖..."
    npm install
fi

echo "正在启动开发服务器..."
echo "请按 Ctrl+C 停止"
npm run dev -- --host
