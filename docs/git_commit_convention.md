# Git 提交规范

本项目使用语义化提交（Conventional Commits）。提交信息应清晰描述变更类型、影响范围和意图，便于后续检索、回滚和生成变更记录。

## 格式

```text
<type>(<scope>): <subject>
```

`scope` 可选；`subject` 使用英文或中文均可，但要简短明确。正文和页脚仅在需要解释背景、迁移步骤或破坏性变更时添加。

## 常用类型

| type | 使用场景 |
|------|----------|
| `feat` | 新功能、新能力、新接口 |
| `fix` | 修复 bug 或运行时错误 |
| `docs` | 文档、注释、规范、README |
| `style` | 纯格式调整，不改变行为 |
| `refactor` | 重构，不新增功能也不修复 bug |
| `perf` | 性能优化 |
| `test` | 测试用例、测试脚本、验证流程 |
| `build` | CMake、工具链、依赖、构建脚本 |
| `ci` | CI/CD 配置 |
| `chore` | 维护性工作、清理、元数据 |
| `revert` | 回滚提交 |

## Scope 建议

优先使用模块名或影响面：

- `producer`
- `visiong`
- `simple-ipc`
- `stream`
- `rtsp`
- `webrtc`
- `file`
- `http`
- `frontend`
- `cmake`
- `deploy`
- `docs`

## 示例

```text
docs: add agent standards and debug skill
fix(visiong): release python runtime callback on deinit
build(cmake): make sdk sysroot configurable
feat(http): add python project deployment endpoint
test(debug): add mode switch regression workflow
```

## 规则

- `subject` 不以句号结尾。
- 一次提交只表达一个主要意图；文档更新和代码修复应尽量分开提交。
- 修复硬编码、构建路径、部署路径时使用 `build(...)` 或 `fix(...)`，不要用笼统的 `chore`。
- 如果包含破坏性变更，在正文或页脚写明 `BREAKING CHANGE:`。
- 提交前运行与变更相关的验证命令，并在最终说明里记录结果。
