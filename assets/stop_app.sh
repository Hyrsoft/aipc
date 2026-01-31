#!/bin/sh
#
# AIPC 停止脚本
# 用于在 Luckfox Pico 设备上停止 AIPC 应用
#
# 使用方法:
#   ./stop_app.sh [options]
#
# 选项:
#   --force, -f     强制终止（使用 SIGKILL）
#   --help, -h      显示帮助
#

set -e

APP_NAME="aipc"
PID_FILE="/var/run/${APP_NAME}.pid"

# 默认选项
FORCE_KILL=0

# 解析命令行参数
while [ $# -gt 0 ]; do
    case "$1" in
        --force|-f)
            FORCE_KILL=1
            shift
            ;;
        --help|-h)
            echo "AIPC 停止脚本"
            echo ""
            echo "使用方法: $0 [options]"
            echo ""
            echo "选项:"
            echo "  --force, -f     强制终止（使用 SIGKILL）"
            echo "  --help, -h      显示帮助"
            exit 0
            ;;
        *)
            echo "未知选项: $1"
            exit 1
            ;;
    esac
done

echo "=================================================="
echo "       AIPC - 停止服务"
echo "=================================================="
echo ""

# 查找进程
find_aipc_pid() {
    # 首先检查 PID 文件
    if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE")
        if kill -0 "$PID" 2>/dev/null; then
            echo "$PID"
            return
        fi
    fi
    
    # 如果 PID 文件不存在或无效，尝试通过进程名查找
    pgrep -x "$APP_NAME" 2>/dev/null || true
}

PID=$(find_aipc_pid)

if [ -z "$PID" ]; then
    echo "AIPC 服务未在运行"
    # 清理可能残留的 PID 文件
    rm -f "$PID_FILE"
    exit 0
fi

echo "找到 AIPC 进程 (PID: $PID)"

# 停止进程
if [ "$FORCE_KILL" -eq 1 ]; then
    echo "强制终止进程..."
    kill -9 "$PID" 2>/dev/null || true
else
    echo "发送 SIGTERM 信号..."
    kill -15 "$PID" 2>/dev/null || true
    
    # 等待进程退出（最多 10 秒）
    WAIT_COUNT=0
    while kill -0 "$PID" 2>/dev/null && [ $WAIT_COUNT -lt 10 ]; do
        echo "等待进程退出... ($WAIT_COUNT/10)"
        sleep 1
        WAIT_COUNT=$((WAIT_COUNT + 1))
    done
    
    # 如果进程还在运行，强制终止
    if kill -0 "$PID" 2>/dev/null; then
        echo "进程未响应，强制终止..."
        kill -9 "$PID" 2>/dev/null || true
    fi
fi

# 清理 PID 文件
rm -f "$PID_FILE"

# 验证进程已停止
sleep 1
if kill -0 "$PID" 2>/dev/null; then
    echo "警告: 进程可能仍在运行"
    exit 1
else
    echo ""
    echo "AIPC 服务已停止"
fi

echo ""
echo "=================================================="
