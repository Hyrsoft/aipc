# 依赖库管理

## 默认策略

依赖库管理由 daemon 配置中的 `dependencies` 段控制，官方镜像默认关闭：

```json
{
  "enabled": false,
  "root": "/userdata/aipc-rust/data/dependencies",
  "max_upload_bytes": 33554432
}
```

关闭时 `GET /api/v1/dependencies` 仍返回白名单、factory 版本和 `enabled=false`；
上传、删除、激活、回滚和恢复 factory 均返回 HTTP 403。打开该开关等同允许用户向
受限进程加载原生代码，WebUI 激活前必须再次确认。

## 白名单和影响范围

| ID | SONAME | owner | 影响 |
| --- | --- | --- | --- |
| `rknn-runtime` | `librknnmrt.so` | AI | 仅冷重启 `ai_worker` |
| `visiong` | `libvisiong.so` | AI | 仅冷重启 `ai_worker` |
| `rockiva` | `librockiva.so` | AI | 仅冷重启 `ai_worker` |
| `rve` | `librve.so` | AI | 仅冷重启 `ai_worker` |
| `ivs` | `libivs.so` | AI | 仅冷重启 `ai_worker` |
| `rga` | `librga.so` | Media + AI | 停止 AI，重启 media，再恢复 AI |
| `mpp` | `librockchip_mpp.so.1` | Media + AI | 停止 AI，重启 media，再恢复 AI |
| `rkaiq` | `librkaiq.so` | Media + AI | 停止 AI，重启 media，再恢复 AI |
| `rockit` | `librockit.so` | Media + AI | 停止 AI，重启 media，再恢复 AI |

不接受任意文件名，也不开放 libc、libstdc++、SSL 或 daemon 自身依赖。版本以
`sha256` 内容寻址，语义版本只是可选展示字段；无法解析语义版本时以 Build ID 和
SHA-256 为权威标识。RKNN runtime 会提取其内嵌的 runtime 版本。

## 存储和切换流程

上传版本位于：

```text
/userdata/aipc-rust/data/dependencies/versions/<library>/<sha256>/
  <soname>
  version.json
/userdata/aipc-rust/data/dependencies/active/<library> -> ../versions/...
state.json
```

上传先写隐藏 `.part` 目录，完成 SHA-256、ELF32 little-endian ARM/EABI、`ET_DYN`、
SONAME、NEEDED、Build ID 和大小检查后再原子 rename，并同步目录。候选库由独立
worker 使用 `--probe-load` 预检，daemon 自身从不 `dlopen` 用户库。切换 active 指针
后执行完整 readiness 检查；AI 项目还必须收到 Ready 和首次推理。任何失败都会切回
previous；没有 previous 时使用 factory。回滚失败会保留 daemon、Web API 和 watchdog，
并将库标记为 `degraded`、把错误写入 `state.json` 和事件流。

配置应用、AI 部署、worker restart 和依赖切换共用 maintenance gate；并发操作返回
HTTP 409，避免两个操作同时修改 active 指针或硬件资源。

## API

```text
GET    /api/v1/dependencies
POST   /api/v1/dependencies/{id}/versions   multipart field: file
DELETE /api/v1/dependencies/{id}/versions/{sha256}
POST   /api/v1/dependencies/{id}/activate  JSON: {"sha256":"..."}
POST   /api/v1/dependencies/{id}/rollback
POST   /api/v1/dependencies/{id}/factory
```

返回内容包含 `id`、加载名、owner、factory/active/previous、候选版本的字节数、
SHA-256、SONAME、Build ID、检测版本、状态和最近错误。激活接口只在健康检查完成或
自动回滚完成后返回。

## WebUI

系统配置页的“依赖库”面板展示版本来源、哈希、影响进程和运行状态，支持上传候选、
激活、回滚、恢复 factory 以及删除未使用版本。禁用时仅展示启用方法，不发送变更请求。

## RV1106 验证

2026-08-08 在 `192.168.100.112`（ADB `51b2f225656e6459`）验证：

- factory `librknnmrt.so` 报告 runtime `2.3.2 (429f97ae6b@2025-04-09T09:11:49)`；
- 激活内容相同的 RKNN runtime：media PID/generation 不变，AI PID/generation 改变，
  首次推理成功；
- 激活 factory `librga.so`：media 和 AI 均冷重启，`yolov5-coco80` 自动恢复，preview、
  RTSP、录像和 `ffprobe` 均通过；
- 将 `librga.so` 上传到 `rknn-runtime` 因 SONAME 不匹配返回 HTTP 400；
- 一次 factory 恢复触发硬件锁死，watchdog 在约 44 秒后复位。复位后 rootfs 为只读，
  因此持久化数据、日志、录像和 active override 已迁移到可写 `/userdata`。

板端包默认部署到 `/userdata/aipc-rust`。若 `/etc/init.d` 因保护性只读无法更新，需在
SDK 镜像中安装 `S99aipc -> /userdata/aipc-rust/scripts/init.sh`；部署脚本会给出警告，
不会假装已安装开机入口。
