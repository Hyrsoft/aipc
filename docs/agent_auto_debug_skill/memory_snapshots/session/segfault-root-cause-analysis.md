# Segfault Root Cause Analysis Snapshot

## 冷切换时序
- /api/ai/switch -> MediaManager::SwitchMode(VisionG)
- stop/deinit old producer
- create/init new VisionG producer
- re-register consumers
- start frame loop

## 高概率崩溃点
1. frame loop 与 runtime 销毁并发
2. GIL 与 interpreter/runtime 生命周期冲突
3. UpdateCode 与 ProcessFrame 竞争
4. producer 指针在切换边界悬挂
5. 多次切换导致 python runtime 状态污染

## 当前证据
- interpreter 初始化可完成
- 崩溃更靠近 LoadCode -> py::exec 阶段
- 说明不是纯解释器启动即崩

## 建议策略
- runtime 生命周期改为共享引用或严格单向销毁
- Stop 后确认 frame loop 完整退出再 reset runtime
- 统一 UpdateCode/ProcessFrame 门控
- 切换与 deploy 请求边界做线程安全保护

## 验证
- 连续切换压测
- deploy 并发压测
- 失败路径可恢复性验证
