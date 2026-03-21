---
name: aipc-agent-auto-debug
description: AIPC 远程联调与 VisionG 切换崩溃自动排查流程。用于复现、缩小范围、记录证据、形成可回归验证清单。
---

# AIPC Agent 自动调试 Skill

## 使用场景
- 远程设备 SSH 可达，但问题只能在板端复现。
- SimpleIPC 与 VisionG 冷切换后服务退出、端口拒绝连接、疑似段错误。
- Python 工程部署后偶发崩溃，需要快速判断在初始化、LoadCode 还是帧循环。

## 调试原则
1. 先保活再定位：先确认进程、端口、日志，再做功能验证。
2. 一次只改一个变量：脚本内容、切换时机、线程上下文不要同时改。
3. 保留证据链：每轮保留 API 请求、关键日志、进程状态。
4. 先最小脚本，后恢复业务脚本：避免被复杂依赖干扰。

## 标准流程
1. 基线检查
- 检查进程是否存在。
- 检查 8080 状态接口是否可访问。
- 检查最近日志是否有 switch/deploy 记录。

2. 复现最小闭环
- 触发 /api/ai/switch -> visiong。
- 立即读取 /api/status。
- 抓取日志关键字：switch、VisionG producer、PythonInit、LoadCode。

3. 最小脚本二分
- 第一步脚本只保留 process(): return None。
- 第二步加入 import visiong。
- 第三步加入 init()，但不创建 Camera/NPU。
- 第四步逐步加入 Camera/NPU。
- 每一步都执行 deploy 并记录是否崩溃。

4. 线程与生命周期验证
- 验证 Stop/Deinit 是否在 frame loop 完整退出后执行。
- 验证 UpdateCode 与 ProcessFrame 是否受同一状态门控。
- 验证 runtime 对象的创建与销毁只在安全边界发生。

5. 回归标准
- 连续 50+ 次切换不崩。
- deploy 成功/失败都不导致服务退出。
- /api/status 与 /api/python/status 在失败后仍可响应。

## 推荐日志关键字
- AI model switch requested
- Mode switch
- Initializing VisionG producer
- VisionG producer initialized
- [PythonInit]
- LoadCode
- Failed to load initial Python code

## 输出模板
- 问题现象：
- 复现命令：
- 关键日志：
- 已排除项：
- 高概率根因：
- 下一步最小改动：
