---
name: aipc-agent-auto-debug
description: Build, deploy, run, and debug this AIPC project on RV1106/Luckfox hardware using either ADB port forwarding or network SSH/HTTP. Use when validating board behavior, mode switching, stream recovery, Python VisionG deployment, or regressions after code changes.
---

# AIPC 自动化调试 Skill

## 目标

在本仓库中自动完成 AIPC 的编译、部署、启动、健康检查、模式切换回归和日志采集。优先使用 ADB；如果设备没有 ADB 或需要完整部署，则使用网络 SSH/rsync。

## 参数约定

默认值可由环境变量覆盖：

```bash
AIPC_BUILD_DIR=${AIPC_BUILD_DIR:-build/Debug}
AIPC_REMOTE_HOST=${AIPC_REMOTE_HOST:-root@192.168.8.235}
AIPC_REMOTE_DIR=${AIPC_REMOTE_DIR:-/root/aipc}
AIPC_HTTP_BASE=${AIPC_HTTP_BASE:-http://192.168.8.235:8080}
AIPC_ADB_FORWARD_PORT=${AIPC_ADB_FORWARD_PORT:-18080}
```

不要把新的固定 IP、用户名、绝对 SDK 路径写入代码。需要变更目标设备时，通过这些环境变量或命令参数传入。

## 通道选择

1. 先执行 `adb devices`。如果能看到设备，优先用 ADB：
   - `adb shell` 检查进程和日志。
   - `adb push` 上传本地构建产物或安装目录。
   - `adb forward tcp:${AIPC_ADB_FORWARD_PORT} tcp:8080` 后用 `http://127.0.0.1:${AIPC_ADB_FORWARD_PORT}` 调 API。
2. 如果 ADB 不可用，使用网络：
   - `ssh ${AIPC_REMOTE_HOST} ...`
   - `rsync -az --delete ... ${AIPC_REMOTE_HOST}:${AIPC_REMOTE_DIR}/`
   - `curl ${AIPC_HTTP_BASE}/api/status`

## 标准流程

### 1. 编译

```bash
cmake --build "${AIPC_BUILD_DIR}"
```

通过标准：命令退出码为 0，产物 `${AIPC_BUILD_DIR}/bin/aipc` 存在。

### 2. 安装打包

前端或安装内容可能变化时：

```bash
./assets/build_frontend.sh
cmake --install "${AIPC_BUILD_DIR}"
```

安装目录默认为 `${AIPC_BUILD_DIR}/install`。

### 3. 部署

ADB 路径：

```bash
adb shell "mkdir -p ${AIPC_REMOTE_DIR}"
adb push "${AIPC_BUILD_DIR}/install/." "${AIPC_REMOTE_DIR}/"
```

网络路径：

```bash
rsync -az --delete "${AIPC_BUILD_DIR}/install/" "${AIPC_REMOTE_HOST}:${AIPC_REMOTE_DIR}/"
```

如果使用仓库脚本，优先设置环境变量后执行：

```bash
AIPC_REMOTE_HOST="${AIPC_REMOTE_HOST}" AIPC_REMOTE_DIR="${AIPC_REMOTE_DIR}" ./assets/install_rsync.sh
```

### 4. 启动或重启

ADB 路径：

```bash
adb shell "${AIPC_REMOTE_DIR}/bin/stop_app.sh 2>/dev/null || pkill -x aipc || true"
adb shell "cd ${AIPC_REMOTE_DIR}/bin && nohup env LD_LIBRARY_PATH=${AIPC_REMOTE_DIR}/lib:\$LD_LIBRARY_PATH ./aipc > /var/log/aipc.log 2>&1 &"
adb shell "pgrep -x aipc"
```

网络路径：

```bash
ssh "${AIPC_REMOTE_HOST}" "${AIPC_REMOTE_DIR}/bin/stop_app.sh 2>/dev/null || pkill -x aipc || true"
ssh "${AIPC_REMOTE_HOST}" "cd ${AIPC_REMOTE_DIR}/bin && nohup env LD_LIBRARY_PATH=${AIPC_REMOTE_DIR}/lib:\$LD_LIBRARY_PATH ./aipc > /var/log/aipc.log 2>&1 &"
ssh "${AIPC_REMOTE_HOST}" "pgrep -x aipc"
```

### 5. HTTP 健康检查

ADB 路径先转发端口：

```bash
adb forward --remove "tcp:${AIPC_ADB_FORWARD_PORT}" 2>/dev/null || true
adb forward "tcp:${AIPC_ADB_FORWARD_PORT}" tcp:8080
AIPC_HTTP_BASE="http://127.0.0.1:${AIPC_ADB_FORWARD_PORT}"
```

检查：

```bash
curl -fsS "${AIPC_HTTP_BASE}/api/status"
curl -fsS "${AIPC_HTTP_BASE}/api/python/status"
curl -fsS "${AIPC_HTTP_BASE}/api/rtsp/status"
```

通过标准：

- `/api/status` 返回 JSON 且 `success=true`。
- producer 存在并处于 running。
- 进程未退出。

### 6. 模式切换回归

执行 SimpleIPC -> VisionG -> SimpleIPC：

```bash
curl -fsS -X POST "${AIPC_HTTP_BASE}/api/ai/switch" \
  -H "Content-Type: application/json" \
  -d '{"model":"visiong"}'

curl -fsS "${AIPC_HTTP_BASE}/api/python/status"

curl -fsS -X POST "${AIPC_HTTP_BASE}/api/ai/switch" \
  -H "Content-Type: application/json" \
  -d '{"model":"none"}'

curl -fsS "${AIPC_HTTP_BASE}/api/status"
```

通过标准：

- 切到 VisionG 后 `/api/python/status.data.active=true`。
- 切回 none 后 producer mode 为 SimpleIPC。
- HTTP 服务不中断，进程仍存活。

### 7. 日志采集

ADB：

```bash
adb shell "tail -n 160 /var/log/aipc.log"
adb shell "grep -E 'Mode switch|SimpleIPC producer started|VisionG|Code error|encode failed|VENC' /var/log/aipc.log | tail -120"
```

网络：

```bash
ssh "${AIPC_REMOTE_HOST}" "tail -n 160 /var/log/aipc.log"
ssh "${AIPC_REMOTE_HOST}" "grep -E 'Mode switch|SimpleIPC producer started|VisionG|Code error|encode failed|VENC' /var/log/aipc.log | tail -120"
```

## 故障定位优先级

1. 进程是否存在：`pgrep -x aipc`。
2. HTTP 是否可达：`/api/status`。
3. 当前 producer 模式和 running 状态。
4. 最近日志中的 `Mode switch`、`Code error`、`encode failed`。
5. 若只有 ADB 可用，确认端口转发是否指向 8080。
6. 若只有网络可用，确认设备 IP、SSH、rsync 和防火墙。

## 输出格式

每次调试结束输出：

```text
- transport: adb|network
- build: pass|fail
- deploy: pass|fail
- process: running|stopped
- http: pass|fail
- mode switch: pass|fail
- key logs:
- root cause:
- next action:
```
