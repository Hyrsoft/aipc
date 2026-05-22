# AIPC Agent 自动调试资料包

这个目录用于沉淀两类内容：
- 可复用调试流程：SKILL.md
- 可选记忆快照：memory_snapshots/

说明：
- 本目录用于开发与联调文档沉淀，便于后续排查复用。
- 旧 session 级快照已经清理，避免和当前 `MediaManager + SimpleIPC/VisionG` 架构产生冲突。
- 如果希望让 VS Code Agent 自动发现并加载 skill，建议额外复制一份到 .github/skills/<name>/SKILL.md。
