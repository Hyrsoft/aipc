#!/bin/bash

# 设置变量
INSTALL_DIR="build/Debug/install"
REMOTE_HOST="root@192.168.8.235"
REMOTE_PATH="/root/aipc"

# 步骤 0: 构建前端
./assets/build_frontend.sh
if [ $? -ne 0 ]; then
    echo "错误：前端构建失败"
    exit 1
fi

# 第一步：执行 cmake install
echo "========================================="
echo "执行 cmake --install build/Debug..."
echo "========================================="
cmake --install build/Debug
if [ $? -ne 0 ]; then
    echo "错误：cmake install 失败"
    exit 1
fi

# 第一步半：删除 libdatachannel 相关文件（已手动部署到设备）
echo ""
echo "清理 libdatachannel 的动态库和头文件..."
rm -f "$INSTALL_DIR/lib/libdatachannel.so"*
rm -f "$INSTALL_DIR/lib/libdatachannel.a"
rm -rf "$INSTALL_DIR/include"
rm -rf "$INSTALL_DIR/lib"
echo "清理完成"

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
