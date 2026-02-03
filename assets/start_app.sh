#!/bin/sh
#
# AIPC 启动脚本
# 用于在 Luckfox Pico 设备上启动 AIPC 应用
#
# 使用方法:
#   ./start_app.sh [options]
#
# 选项:
#   --record, -r    启用录制功能
#   --no-rtsp       禁用 RTSP 流
#   --no-webrtc     禁用 WebRTC 流
#   --daemon, -d    后台运行
#   --help, -h      显示帮助
#

set -e

# 获取脚本所在目录
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
APP_NAME="aipc"
PID_FILE="/var/run/${APP_NAME}.pid"
LOG_FILE="/var/log/${APP_NAME}.log"

# 默认选项
DAEMON_MODE=0
EXTRA_ARGS=""

# 解析命令行参数
while [ $# -gt 0 ]; do
    case "$1" in
        --daemon|-d)
            DAEMON_MODE=1
            shift
            ;;
        --record|-r|--no-rtsp|--no-webrtc)
            EXTRA_ARGS="$EXTRA_ARGS $1"
            shift
            ;;
        --help|-h)
            echo "AIPC 启动脚本"
            echo ""
            echo "使用方法: $0 [options]"
            echo ""
            echo "选项:"
            echo "  --record, -r    启用录制功能"
            echo "  --no-rtsp       禁用 RTSP 流"
            echo "  --no-webrtc     禁用 WebRTC 流"
            echo "  --daemon, -d    后台运行"
            echo "  --help, -h      显示帮助"
            echo ""
            echo "环境变量:"
            echo "  SIGNALING_HOST  WebRTC 信令服务器地址 (默认: 127.0.0.1)"
            exit 0
            ;;
        *)
            EXTRA_ARGS="$EXTRA_ARGS $1"
            shift
            ;;
    esac
done

# 检查是否已经在运行
if [ -f "$PID_FILE" ]; then
    OLD_PID=$(cat "$PID_FILE")
    if kill -0 "$OLD_PID" 2>/dev/null; then
        echo "错误: AIPC 已经在运行 (PID: $OLD_PID)"
        echo "请先运行 stop_app.sh 停止服务"
        exit 1
    else
        # PID 文件存在但进程不存在，删除旧的 PID 文件
        rm -f "$PID_FILE"
    fi
fi

# 设置库路径
export LD_LIBRARY_PATH="$SCRIPT_DIR/../lib:$SCRIPT_DIR:$LD_LIBRARY_PATH"

# 切换到脚本目录
cd "$SCRIPT_DIR"

echo "=================================================="
echo "       AIPC - 边缘 AI 相机"
echo "=================================================="
echo ""
echo "正在启动服务..."
echo ""

# 检查可执行文件
if [ ! -x "./${APP_NAME}" ]; then
    echo "错误: 找不到可执行文件 ${APP_NAME}"
    exit 1
fi

# 停止默认的 rkipc（如果存在）
if [ -x "/oem/usr/bin/RkLunch-stop.sh" ]; then
    echo "停止默认 rkipc 服务..."
    /oem/usr/bin/RkLunch-stop.sh 2>/dev/null || true
fi

# 启动应用
# 注意：HTTP 服务器已内置于 aipc 应用中，会自动提供 Web UI (端口 8080)
if [ "$DAEMON_MODE" -eq 1 ]; then
    echo "以后台模式启动..."
    nohup "./${APP_NAME}" $EXTRA_ARGS > "$LOG_FILE" 2>&1 &
    APP_PID=$!
    echo $APP_PID > "$PID_FILE"
    echo ""
    echo "AIPC 已在后台启动 (PID: $APP_PID)"
    echo "日志文件: $LOG_FILE"
    echo "停止服务: ./stop_app.sh"
else
    echo "以前台模式启动..."
    echo "按 Ctrl+C 停止服务"
    echo ""
    "./${APP_NAME}" $EXTRA_ARGS
fi

echo ""
echo "服务信息:"
echo "  HTTP API:   http://<设备IP>:8080/api/status"
echo "  控制面板:   http://<设备IP>:8080/admin.html"
echo "  WebRTC:     http://<设备IP>:8080/index.html"
echo "  RTSP 流:    rtsp://<设备IP>:554/live/0"
echo "  信令服务:   ws://<设备IP>:8000"
echo ""
echo "=================================================="
