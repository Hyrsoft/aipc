# AIPC 单核 CPU 优化重构

## 概述

本次重构针对 RV1106 单核 CPU 进行优化，引入 Asio 异步 I/O 库，重新设计了视频流分发架构。

## 线程模型变化

### 重构前（多线程 + 队列）

```
[Hardware VENC]
      |
      v
[Fetch Thread] ── push ──> [Queue1] ──> [RTSP Dispatch Thread]
                  push ──> [Queue2] ──> [WebRTC Dispatch Thread]
                  push ──> [Queue3] ──> [WS Dispatch Thread]
                  push ──> [Queue4] ──> [File Dispatch Thread]

总线程数：5+ (Fetch + 4个分发线程)
问题：单核CPU上频繁上下文切换，Cache Thrashing
```

### 重构后（直接分发 + Asio）

```
[Hardware VENC]
      |
      v
[Video Fetch Thread] ← 唯一的视频获取线程
      |
      | (直接调用消费者回调)
      |
  ┌───┼───┬───────┬───────┐
  │   │   │       │       │
  │   │   │       │       │
 RTSP WS WebRTC  File
(post)(post)(post)(queue)
  │   │   │       │
  └───┴───┘       │
      │           │
      v           v
[Main IO Thread] [File Thread]
 (asio事件循环)   (独立线程)

总线程数：3 (Fetch + Main IO + File)
优化：减少 60%+ 线程数，消除无意义的上下文切换
```

## 新增文件

### 1. `src/common/asio_context.h`
全局 Asio IO Context 管理，提供：
- 单例模式的 `io_context`
- `Post()` / `Dispatch()` 方法投递任务
- `Run()` 在主线程运行事件循环

### 2. `src/rkvideo/stream_dispatcher.h` (重写)
新的流分发器，支持三种消费者类型：
- `ConsumerType::Direct` - 直接在 Fetch 线程执行（极快操作）
- `ConsumerType::AsyncIO` - 投递到 IO 线程执行（网络发送）
- `ConsumerType::Queued` - 独立队列+线程（文件写入）

## 修改文件

### `CMakeLists.txt`
- 添加 Asio 依赖配置
- 添加 `ASIO_STANDALONE` 和 `ASIO_NO_DEPRECATED` 编译定义

### `src/common/CMakeLists.txt`
- 链接 asio 库

### `src/rkvideo/rkvideo.h`
- 添加 `ConsumerType` 参数到注册接口

### `src/rkvideo/rkvideo.cpp`
- 更新消费者注册逻辑支持类型

### `src/thread_stream.cpp`
- RTSP、WebRTC、WebSocket 使用 `AsyncIO` 类型
- File 使用 `Queued` 类型

### `src/main.cpp`
- 主循环改为 `aipc::IoContext::Instance().Run()`
- 信号处理中停止 IO 循环

## 性能优化点

1. **减少线程数**：从 5+ 减少到 3
2. **消除中间队列**：网络消费者不再需要独立队列
3. **减少锁竞争**：直接分发，无需队列锁
4. **更好的 Cache 利用**：更少的线程意味着更少的 Cache Thrashing
5. **减少上下文切换**：单核 CPU 最大收益点

## 使用方式

消费者回调现在有两种执行上下文：

```cpp
// 网络消费者（AsyncIO）- 回调在主 IO 线程执行
void RtspThread::StreamConsumer(EncodedStreamPtr stream, void* user_data) {
    // 这里已经在 IO 线程中，可以直接发送
    // 不需要额外的 post
}

// 文件消费者（Queued）- 回调在独立线程执行
void FileThread::StreamConsumer(EncodedStreamPtr stream, void* user_data) {
    // 这里在独立的 File 线程中，可以阻塞写入
}
```

## 后续优化方向

1. **HTTP Server**：当前保留 cpp-httplib 线程池模式，未来可考虑替换为 Beast
2. **RTSP**：当前 RTSP 库自带线程，可考虑集成到 Asio
3. **WebRTC**：libdatachannel 自带事件循环，需要研究如何与 Asio 集成
