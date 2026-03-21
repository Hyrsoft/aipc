# Session Plan Snapshot

## 目标
同时解决：
1. 编辑器语义文案不匹配
2. 提供 AIPC 适配 YOLOv5 预设工程
3. 修复 SimpleIPC -> VisionG 冷切换段错误

## 执行阶段
- Phase A: 增强观测点并稳定复现
- Phase B: VisionGProducer 生命周期与并发修复
- Phase C: 预设工程与文案统一
- Phase D: 文档对齐
- Phase E: 压测与回归验证

## 关键验证
- 多轮冷切换无崩溃
- deploy 并发下无崩溃
- 失败路径返回可读错误且进程不退出
- RTSP/WebRTC/WS 在部署后正常
