# AIPC Agent 自动调试 Skill 说明

当前项目级自动化调试 skill 位于：

```text
.github/skills/aipc-agent-auto-debug/SKILL.md
```

该 skill 支持两种调试通道：

- ADB：用于端口转发、进程检查、日志采集、快速推送。
- 网络：用于 SSH、rsync、HTTP API 调试。

## 环境变量

默认值可覆盖，不需要修改脚本源码：

```bash
export AIPC_BUILD_DIR="${AIPC_BUILD_DIR:-build/Debug}"
export AIPC_REMOTE_HOST="${AIPC_REMOTE_HOST:-root@192.168.8.235}"
export AIPC_REMOTE_DIR="${AIPC_REMOTE_DIR:-/root/aipc}"
export AIPC_HTTP_BASE="${AIPC_HTTP_BASE:-http://192.168.8.235:8080}"
export AIPC_ADB_FORWARD_PORT="${AIPC_ADB_FORWARD_PORT:-18080}"
```

## 最小流程

```bash
cmake --build "$AIPC_BUILD_DIR"
./assets/build_frontend.sh
cmake --install "$AIPC_BUILD_DIR"
AIPC_REMOTE_HOST="$AIPC_REMOTE_HOST" AIPC_REMOTE_DIR="$AIPC_REMOTE_DIR" ./assets/install_rsync.sh
curl -fsS "$AIPC_HTTP_BASE/api/status"
```

完整的部署、启动、模式切换、日志采集流程以 `.github/skills/aipc-agent-auto-debug/SKILL.md` 为准。
