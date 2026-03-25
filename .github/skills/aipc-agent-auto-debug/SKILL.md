# AIPC Agent 自动调试 Skill

## 元信息

```yaml
name: aipc-agent-auto-debug
version: 2.0.0
description: >
    AIPC 编译→部署→板端执行→HTTP 接口调试全流程自动化 Skill。
    适用于 VisionG/Python 切换崩溃、Python 工程部署死锁、帧循环异常等场景。
target_device: root@192.168.8.235
device_password: luckfox
device_app_path: /root/aipc
host_os: Arch Linux
ssh_auth: 免密（SSH key 已安装）
http_base: http://192.168.8.235:8080
build_dir: build/Debug
```

---

## 使用场景

- 修改完 C++ 代码后，需要一键编译、部署、重启验证。
- VisionG 模式切换后服务卡死（进程存在但 HTTP 无响应）。
- Python 工程部署后偶发崩溃，需二分定位出错步骤。
- 需要连续切换压测，验证修复是否彻底消除死锁。

---

## 调试原则

1. **先保活再定位**：先确认进程、端口、日志再做功能验证。
2. **一次只改一个变量**：脚本内容、切换时机、线程上下文不要同时改。
3. **保留证据链**：每轮保留 API 请求、关键日志、进程状态快照。
4. **最小脚本优先**：排查时先用最小 Python 工程，逐步加入依赖，缩小爆炸半径。

---

## 标准流程

### Phase 0：编译

```bash
# 在开发机项目根目录执行
cmake --build build/Debug 2>&1 | tail -20
```

判断标准：

- 输出 `[X/X] Linking CXX executable bin/aipc` 且无 `error:` 行 → 编译成功，进入 Phase 1。
- 存在 `error:` → 先修复编译错误，不进入后续步骤。

---

### Phase 1：部署到板端

```bash
# 在开发机项目根目录执行（包含前端构建 + cmake install + rsync）
./assets/install_rsync.sh
```

脚本执行内容（按顺序）：

1. `./assets/build_frontend.sh` — 构建 Web UI 静态文件
2. `cmake --install build/Debug` — 安装到 `build/Debug/install/`
3. 删除已单独部署在设备上的 `libdatachannel.so*`（避免覆盖）
4. `rsync -avz --delete build/Debug/install/ root@192.168.8.235:/root/aipc/`

判断标准：

- 最后一行输出 `完成！文件已同步到远程主机` → 部署成功。
- `rsync` 返回非 0 → 检查网络连通性：`ping 192.168.8.235`。

---

### Phase 2：重启板端服务

```bash
# 停止旧进程
ssh root@192.168.8.235 '/root/aipc/bin/stop_app.sh 2>/dev/null || pkill -x aipc || true'

# 等待进程完全退出
ssh root@192.168.8.235 'sleep 1 && pgrep aipc && echo "WARN: still running" || echo "OK: stopped"'

# 前台启动（日志实时输出，调试用）
ssh root@192.168.8.235 'cd /root/aipc/bin && LD_LIBRARY_PATH=/root/aipc/lib:$LD_LIBRARY_PATH ./aipc' &
SSH_PID=$!

# 等待服务就绪（HTTP 端口 8080）
sleep 3
```

后台运行（稳定性测试用，不需要实时日志时）：

```bash
ssh root@192.168.8.235 'cd /root/aipc/bin && nohup env LD_LIBRARY_PATH=/root/aipc/lib:$LD_LIBRARY_PATH ./aipc > /var/log/aipc.log 2>&1 &'
```

---

### Phase 3：基线健康检查

依次执行以下检查，任意一步失败则停止并报告：

```bash
DEVICE="192.168.8.235"
BASE="http://${DEVICE}:8080"

# 3.1 进程存活
ssh root@${DEVICE} 'pgrep -x aipc && echo "PROC: ok" || echo "PROC: NOT RUNNING"'

# 3.2 HTTP 可达
curl -sf --max-time 5 "${BASE}/api/status" | python3 -m json.tool || echo "HTTP: unreachable"

# 3.3 当前模式
curl -sf "${BASE}/api/status" | python3 -c "import sys,json; d=json.load(sys.stdin); print('MODE:', d.get('data',{}).get('producer',{}).get('mode','unknown'))"

# 3.4 Python 状态（VisionG 模式下）
curl -sf "${BASE}/api/python/status" | python3 -m json.tool
```

---

### Phase 4：功能验证

#### 4.1 切换到 VisionG 模式

```bash
curl -sf -X POST http://192.168.8.235:8080/api/ai/switch \
  -H 'Content-Type: application/json' \
  -d '{"model":"visiong"}' | python3 -m json.tool
```

预期响应：

```json
{
    "success": true,
    "message": "AI model switched",
    "data": { "model": "visiong" }
}
```

切换后立即检查 Python 运行时状态：

```bash
sleep 1
curl -sf http://192.168.8.235:8080/api/python/status | python3 -m json.tool
```

#### 4.2 部署 Python 工程

```bash
# 列出可用工程
curl -sf http://192.168.8.235:8080/api/python/projects | python3 -m json.tool

# 部署指定工程（替换 PROJECT_NAME）
curl -sf -X POST http://192.168.8.235:8080/api/python/deploy \
  -H 'Content-Type: application/json' \
  -d '{"project":"PROJECT_NAME.py"}' | python3 -m json.tool
```

预期响应（成功）：

```json
{ "success": true, "message": "Project deployed" }
```

#### 4.3 切换回 SimpleIPC

```bash
curl -sf -X POST http://192.168.8.235:8080/api/ai/switch \
  -H 'Content-Type: application/json' \
  -d '{"model":"none"}' | python3 -m json.tool
```

---

### Phase 5：日志抓取

```bash
# 实时跟踪日志（前台运行时）
ssh root@192.168.8.235 'tail -f /var/log/aipc.log'

# 抓取关键日志关键字
ssh root@192.168.8.235 'grep -E "switch|VisionG|PythonInit|LoadCode|CallRun|Shutdown|error|Error|WARN" /var/log/aipc.log | tail -50'
```

关键日志关键字说明：

| 关键字                                                   | 含义                              |
| -------------------------------------------------------- | --------------------------------- |
| `[PythonInit] initialize_interpreter begin/done`         | Python 解释器初始化（只出现一次） |
| `[PythonInit] Embedded Python ready, GIL released`       | GIL 已释放，多线程可安全获取      |
| `[PythonRuntime] ctor begin/done`                        | PythonRuntime 实例构造            |
| `[LoadCode] begin` / `[LoadCode] committed successfully` | Python 代码加载流程               |
| `[CallRun] calling Python run()`                         | Python run() 开始执行             |
| `[CallRun] Python run() returned normally`               | Python run() 正常退出             |
| `Mode switch: A -> B`                                    | 生产者模式切换                    |
| `VisionG producer initialized`                           | VisionG 初始化完成                |
| `Failed to load initial Python`                          | 初始脚本加载失败，检查语法        |

---

### Phase 6：最小脚本二分排查

当部署后崩溃或卡死，用以下脚本序列逐步缩小范围，每步部署后观察是否崩溃：

**Step 1 — 空壳脚本（验证 Python 运行时本身）**

```python
# minimal_step1.py
def run():
    import aipc
    while aipc.is_running():
        pass
```

**Step 2 — 加入 visiong 导入**

```python
# minimal_step2.py
import visiong

def run():
    import aipc
    while aipc.is_running():
        pass
```

**Step 3 — 加入 init()，但不创建资源**

```python
# minimal_step3.py
import visiong

def init():
    pass  # 不创建 Camera/NPU

def run():
    import aipc
    while aipc.is_running():
        pass

def cleanup():
    pass
```

**Step 4 — 加入 Camera 初始化**

```python
# minimal_step4.py
import visiong, aipc

_cam = None

def init():
    global _cam
    _cam = visiong.Camera(640, 360, format='rgb')

def run():
    while aipc.is_running():
        frame = _cam.snapshot()
        if frame.is_valid():
            aipc.submit_frame(frame.to_format('bgr888'))

def cleanup():
    global _cam
    if _cam:
        _cam.release()
        _cam = None
```

每一步：

1. 通过 `/api/python/projects/{name}` 上传代码。
2. 通过 `/api/python/deploy` 部署。
3. 观察日志，记录是否卡住/崩溃。
4. 如崩溃，回退到上一步，在该步骤内继续二分。

---

### Phase 7：稳定性压测

连续切换 N 次，验证修复是否彻底：

```bash
DEVICE="192.168.8.235"
BASE="http://${DEVICE}:8080"
N=20
FAIL=0

for i in $(seq 1 $N); do
  # 切换到 VisionG
  R=$(curl -sf -X POST "${BASE}/api/ai/switch" \
    -H 'Content-Type: application/json' \
    -d '{"model":"visiong"}')
  echo "[${i}/${N}] visiong: $R"
  sleep 2

  # 检查 HTTP 仍可响应
  if ! curl -sf --max-time 3 "${BASE}/api/status" > /dev/null; then
    echo "FAIL at switch ${i}: HTTP unreachable after visiong switch"
    FAIL=$((FAIL+1))
    break
  fi

  # 切换回 SimpleIPC
  R=$(curl -sf -X POST "${BASE}/api/ai/switch" \
    -H 'Content-Type: application/json' \
    -d '{"model":"none"}')
  echo "[${i}/${N}] simple: $R"
  sleep 2
done

echo "压测完成：失败次数 ${FAIL}/${N}"
```

**通过标准**：连续 20+ 次切换 `FAIL=0`，且 `/api/status` 全程可响应。

---

## 常见故障模式与处置

### 卡在 `[PythonRuntime] ctor begin`，无 `ctor done`

**原因**：`py::gil_scoped_acquire` 阻塞，GIL 被其他线程死持。

**处置**：

1. 检查 `visiong_producer.cpp` 中 `EnsureEmbeddedPythonReady` lambda 末尾是否有 `py::gil_scoped_release`。
2. 在 GDB 中执行 `thread apply all bt`，查找持有 GIL 的线程。

### `LoadCode` 或 `Shutdown` 与 `CallRun` 死锁

**原因**：锁顺序不一致（`mutex_→GIL` vs `GIL→mutex_`）。

**处置**：统一所有路径为 `GIL → mutex_`，参考 `visiong_producer.cpp` L308、L427。

### 部署后 HTTP 立即 502 / 连接拒绝

**原因**：进程已崩溃退出。

**处置**：

```bash
ssh root@192.168.8.235 'pgrep aipc || echo "crashed"'
ssh root@192.168.8.235 'tail -30 /var/log/aipc.log'
```

### rsync 失败

**原因**：网络不通或 SSH 会话问题。

**处置**：

```bash
ping -c 3 192.168.8.235
ssh root@192.168.8.235 echo ok
```

---

## 输出模板

每轮调试结束后填写：
