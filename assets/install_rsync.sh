#!/bin/bash

# 设置变量
INSTALL_DIR="build/Debug/install"
REMOTE_HOST="root@192.168.8.235"
REMOTE_PATH="/root/aipc"

# 第一步：执行 cmake install
echo "========================================="
echo "执行 cmake --install build/Debug..."
echo "========================================="
cmake --install build/Debug
if [ $? -ne 0 ]; then
    echo "错误：cmake install 失败"
    exit 1
fi

# 第二步：使用 rsync 增量同步
echo ""
echo "========================================="
echo "开始增量同步到 $REMOTE_HOST:$REMOTE_PATH..."
echo "========================================="
rsync -avz --delete "$INSTALL_DIR/" "$REMOTE_HOST:$REMOTE_PATH/"
if [ $? -ne 0 ]; then
    echo "错误：rsync 同步失败"
    exit 1
fi

echo ""
echo "========================================="
echo "完成！文件已同步到远程主机 $REMOTE_HOST:$REMOTE_PATH"
echo "========================================="
