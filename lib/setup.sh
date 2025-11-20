#!/bin/bash

# 使用 'set -e' 来确保脚本在任何命令失败时立即退出，这是一种好的编程习惯
set -e

# --- 配置 ---
# 定义要下载的文件的 URL 和本地文件名
FILE_URL="https://github.com/nihui/opencv-mobile/releases/download/v31/opencv-mobile-4.10.0-luckfox-pico.zip"
ZIP_FILE="opencv-mobile-4.10.0-luckfox-pico.zip"
# 定义解压后你期望的文件夹名称
EXTRACTED_DIR="opencv-mobile-4.10.0-luckfox-pico"

# --- 脚本逻辑 ---

# 1. 检查依赖文件夹是否已存在，如果存在则无需重复执行
if [ -d "$EXTRACTED_DIR" ]; then
    echo "依赖文件夹 '$EXTRACTED_DIR' 已存在，跳过安装。"
    exit 0
fi

# 2. 下载压缩包
# 使用 curl 下载文件。-L 选项可以处理重定向。
echo "正在从 $FILE_URL 下载依赖..."
curl -L -o "$ZIP_FILE" "$FILE_URL"

# 3. 检查文件是否下载成功
if [ ! -f "$ZIP_FILE" ]; then
    echo "错误：文件下载失败！"
    exit 1
fi

# 4. 解压文件
echo "正在解压 $ZIP_FILE..."
unzip "$ZIP_FILE"

# 5. 删除不再需要的压缩包
echo "正在删除临时压缩包 $ZIP_FILE..."
rm "$ZIP_FILE"

echo "依赖安装成功！"