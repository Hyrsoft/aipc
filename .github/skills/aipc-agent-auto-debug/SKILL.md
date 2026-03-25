# AIPC Agent 自动调试 Skill（模式切换回归版）

## 元信息

```yaml
name: aipc-agent-auto-debug
version: 3.0.0
description: >
  AIPC 编译→部署→板端运行→模式切换回归测试（SimpleIPC <-> VisionG）自动化流程。
  重点覆盖：首次切换、二次部署、异常脚本回退、连续压测、流恢复验证。
target_device: root@192.168.8.235
http_base: http://192.168.8.235:8080
build_dir: build/Debug
```

---

## 适用场景

- SimpleIPC 切到 VisionG 后，部署报错或前端显示异常。
- VisionG 二次部署后出现编码失败、流中断。
- VisionG 切回 SimpleIPC 后，RTSP/WebRTC/WS 没有帧。
- 需要验证修复后模式切换是否稳定。

---

## 判定总则

- 每一步都要保留证据：请求响应、进程状态、关键日志。
- 先验证服务存活，再验证业务正确性。
- 出错后优先执行“回切到 none + 状态复查”，避免长时间卡死。

---

## Phase 0：编译

```bash
cmake --build build/Debug 2>&1 | tail -20
```

通过标准：出现 `Linking CXX executable bin/aipc` 且无 `error:`。

---

## Phase 1：部署

```bash
./assets/install_rsync.sh
```

通过标准：最后输出 `完成！文件已同步到远程主机`。

---

## Phase 2：重启服务

```bash
ssh root@192.168.8.235 '/root/aipc/bin/stop_app.sh 2>/dev/null || pkill -x aipc || true'
ssh root@192.168.8.235 'sleep 1 && pgrep -x aipc && echo "WARN: still running" || echo "OK: stopped"'
ssh root@192.168.8.235 'cd /root/aipc/bin && nohup env LD_LIBRARY_PATH=/root/aipc/lib:$LD_LIBRARY_PATH ./aipc > /var/log/aipc.log 2>&1 &'
sleep 2
ssh root@192.168.8.235 'pgrep -x aipc && echo "PROC: ok" || echo "PROC: failed"'
```

---

## Phase 3：基线健康检查

```bash
BASE='http://192.168.8.235:8080'

python3 - <<'PY'
import json, urllib.request
BASE='http://192.168.8.235:8080'
for p in ['/api/status','/api/python/status']:
    with urllib.request.urlopen(BASE+p, timeout=5) as r:
        print(p, json.dumps(json.loads(r.read().decode()), ensure_ascii=False))
PY
```

通过标准：
- `/api/status.success=true`
- 当前模式为 `SimpleIPC`
- `/api/python/status.success=true`

---

## Phase 4：模式切换回归矩阵

### Case A：首次切换（SimpleIPC -> VisionG -> none）

1. POST `/api/ai/switch` with `{"model":"visiong"}`
2. GET `/api/python/status`（active 应为 true）
3. POST `/api/ai/switch` with `{"model":"none"}`
4. GET `/api/status`（mode 应为 SimpleIPC，producer.running 应恢复 true）

### Case B：有效脚本二次部署

1. 切到 VisionG
2. 保存并部署带 `run()` 的最小脚本：

```python
def run():
    import aipc
    while aipc.is_running():
        pass
```

3. 观察 `/api/python/deploy` 返回 success=true
4. 再切回 none，确认 HTTP 不中断且进程存活

### Case C：无效脚本容错

1. 切到 VisionG
2. 部署无 `run()` 脚本（例如只有 `process()`）
3. 预期 `/api/python/deploy` 返回 `Code error`
4. 紧接切回 none，确认仍能回到 SimpleIPC 且服务不崩溃

### Case D：连续压测

```bash
python3 - <<'PY'
import json, urllib.request, time
BASE='http://192.168.8.235:8080'
N=20
fail=0

def post(path, body):
    req=urllib.request.Request(BASE+path, data=json.dumps(body).encode(), headers={'Content-Type':'application/json'}, method='POST')
    with urllib.request.urlopen(req, timeout=8) as r:
        return json.loads(r.read().decode())

def get(path):
    with urllib.request.urlopen(BASE+path, timeout=5) as r:
        return json.loads(r.read().decode())

for i in range(1, N+1):
    try:
        post('/api/ai/switch', {'model':'visiong'})
        time.sleep(1)
        get('/api/status')
        post('/api/ai/switch', {'model':'none'})
        time.sleep(1)
        get('/api/status')
        print(f'[{i}/{N}] ok')
    except Exception as e:
        print(f'[{i}/{N}] fail: {e!r}')
        fail += 1
        break

print('FAIL=', fail)
PY
```

通过标准：`FAIL=0`。

---

## Phase 5：流恢复检查（关键）

切回 SimpleIPC 后，执行：

```bash
ssh root@192.168.8.235 'grep -E "Mode switch:|Mode switch completed|SimpleIPC producer started|VisionG] started|encode failed|Code error" /var/log/aipc.log | tail -120'
```

通过标准：
- 有 `Mode switch completed`
- 回切后能看到 `SimpleIPC producer started`
- 无持续刷屏 `Failed to send frame to VENC`

---

## 常见故障与快速定位

### 1) 前端提示 `disconnectWS is not defined`

前端重连函数名调用错误，属于 UI 异常，不是后端 deploy 接口本身崩溃。

### 2) `Code error: Python code must define callable: run()`

部署脚本不符合当前运行时契约，需提供 `run()`。

### 3) 回切 none 后无流

重点检查：
- 回切时是否真正启动了 SimpleIPC（`SimpleIPC producer started`）
- 模式切换之前 VisionG 是否处于异常停止态

---

## 输出模板

每轮调试请输出：

```text
- 版本/提交：
- 测试矩阵：Case A/B/C/D 结果
- 失败步骤：
- 关键日志：
- 根因判断：
- 是否满足回归标准：
- 下一步建议：
```
