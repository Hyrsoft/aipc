# VisionG Integration Analysis Snapshot

## 关键点
- visiong 作为 3rdparty 子模块参与工程
- pybind11::embed 用于 python 嵌入式调用
- visiong_producer_lib 链入 media_producer 再进入 aipc

## 依赖
- visiong runtime libs
- python 3.11 headers/runtime
- pybind11

## 结论
- VisionG 相关崩溃排查重点在 producer 生命周期、python runtime 初始化顺序和脚本执行边界
