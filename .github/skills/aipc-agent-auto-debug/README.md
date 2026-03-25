# AIPC Agent Auto Debug Skill

This folder is for Copilot/Claude skill discovery in workspace scope.

## 用途

本 Skill 定义了 AIPC 项目的自动调试流程，涵盖：

- 编译（cmake --build）
- 部署（install_rsync.sh → rsync 到 root@192.168.8.235）
- 板端启动（SSH 执行 start_app.sh）
- HTTP 接口调试（curl 调用 http://192.168.8.235:8080/api/*）
- 连续切换压测（SimpleIPC ↔ VisionG）
- 崩溃/死锁快速定位

## Source of Truth

**本目录的 SKILL.md 是从 `docs/agent_auto_debug_skill/SKILL.md` 同步过来的副本。**

如需修改，请编辑 `docs/agent_auto_debug_skill/SKILL.md`，然后将其复制到本目录保持同步：

```bash
cp docs/agent_auto_debug_skill/SKILL.md .github/skills/aipc-agent-auto-debug/SKILL.md
```

## 文件说明

| 文件        | 说明                                               |
| ----------- | -------------------------------------------------- |
| `SKILL.md`  | Skill 主体：完整调试流程、脚本、故障模式、输出模板 |
| `README.md` | 本文件，入口说明和同步指引                         |

## 目标设备

| 项目       | 值                                      |
| ---------- | --------------------------------------- |
| SSH 地址   | `root@192.168.8.235`                    |
| 密码       | `luckfox`（已配置免密登录，通常不需要） |
| 应用路径   | `/root/aipc/`                           |
| HTTP API   | `http://192.168.8.235:8080`             |
| 开发机系统 | Arch Linux（SSH 免密插件已安装）        |
