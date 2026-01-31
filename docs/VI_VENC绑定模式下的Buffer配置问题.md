# VI-VENC 绑定模式下的 Buffer 配置问题

## 问题现象

在实现 RTSP 推流功能时，遇到了以下问题：

1. 程序启动后，VENC 只能获取 1-2 帧编码数据
2. 之后 `RK_MPI_VENC_GetStream` 持续超时或阻塞
3. 没有任何错误日志，看起来像是 VENC 不再产生数据

## 排查过程

### 初步怀疑：shared_ptr 跨线程释放问题

最初怀疑是 `RK_MPI_VENC_GetStream` 和 `RK_MPI_VENC_ReleaseStream` 在不同线程中调用导致的死锁：

- FetchLoop 线程调用 `GetStream`
- DispatchLoop 线程中的 shared_ptr 析构时调用 `ReleaseStream`

为此设计了"拷贝后立即释放"的方案：

```cpp
// 方案：在 FetchLoop 中获取数据后立即拷贝，然后同步调用 ReleaseStream
auto frame = acquire_encoded_frame(chn_id, timeout);  // 内部拷贝并释放
```

但问题依然存在。

### 真正原因：VI 通道的 u32Depth 配置错误

通过对比官方示例代码，发现问题在于 VI 通道的配置：

```cpp
// 错误配置
vi_chn_attr.stIspOpt.u32BufCount = 2;
vi_chn_attr.u32Depth = 2;  // ❌ 等于 u32BufCount

// 正确配置
vi_chn_attr.stIspOpt.u32BufCount = 4;
vi_chn_attr.u32Depth = 0;  // ✅ 小于 u32BufCount
```

### 参数说明

| 参数 | 说明 |
|------|------|
| `u32BufCount` | VI 通道的总 buffer 数量 |
| `u32Depth` | 可供 `RK_MPI_VI_GetChnFrame` 获取的帧缓存深度 |

**关键约束**：当 VI 绑定到其他模块（如 VENC）时，`u32Depth` 必须小于 `u32BufCount`。

原因是：
- 绑定模式下，VI 的输出会自动送到下游模块（VENC）
- `u32Depth` 表示保留多少 buffer 供手动获取
- 如果 `u32Depth >= u32BufCount`，所有 buffer 都被"保留"给手动获取，绑定的下游模块就拿不到数据

### u32Depth 的取值建议

| 场景 | u32Depth 推荐值 | 说明 |
|------|-----------------|------|
| 纯绑定模式（VI→VENC） | 0 | 所有数据自动送到 VENC |
| 绑定+手动获取 | 1 | 保留 1 帧供 AI 推理等用途 |
| 纯手动获取 | 1 ~ u32BufCount-1 | 根据处理速度调整 |

## 验证结论

修复 VI 配置后，使用零拷贝方案（shared_ptr 跨线程释放）**可以正常工作**：

```
[media_buffer] VENC GetStream success, seq=70, len=41421
[media_buffer] Releasing VENC stream, seq=70
[media_buffer] VENC ReleaseStream success
[media_buffer] VENC GetStream success, seq=71, len=41755
...
```

这证明：
- ✅ **问题根源是 VI 的 `u32Depth` 配置错误**
- ✅ shared_ptr 跨线程释放 VENC buffer 是可以正常工作的
- ✅ RKMPI 的 `GetStream/ReleaseStream` 不要求在同一线程中调用

## 最终方案

采用零拷贝方案，利用 shared_ptr 的引用计数管理 VENC buffer 生命周期：

```cpp
// media_buffer.h
inline EncodedStreamPtr acquire_encoded_stream(RK_S32 chn_id, RK_S32 timeout_ms) {
    auto stream = new VENC_STREAM_S();
    stream->pstPack = new VENC_PACK_S();
    
    if (RK_MPI_VENC_GetStream(chn_id, stream, timeout_ms) != RK_SUCCESS) {
        delete stream->pstPack;
        delete stream;
        return nullptr;
    }
    
    // 自定义删除器，引用计数归零时自动释放
    return EncodedStreamPtr(stream, [chn_id](VENC_STREAM_S* p) {
        if (p) {
            RK_MPI_VENC_ReleaseStream(chn_id, p);
            delete p->pstPack;
            delete p;
        }
    });
}
```

### 优点

1. **零拷贝**：RTSP/WebRTC 等多个消费者共享同一块 VENC buffer
2. **自动管理**：无需手动追踪 buffer 生命周期
3. **背压处理**：MediaQueue 满时自动丢弃旧帧，shared_ptr 析构触发 ReleaseStream

## 经验总结

1. **仔细阅读 SDK 文档中的参数约束**，特别是绑定模式下的特殊要求
2. **从官方示例中提取关键配置**，对比检查自己的代码
3. **不要过早假设问题原因**，通过日志和实验验证每一个假设
4. **RKMPI 的线程安全性比预期的要好**，shared_ptr 跨线程释放是可行的

## 相关文件

- [luckfox_mpi.cpp](../src/rkvideo/luckfox_mpi.cpp) - VI/VENC 初始化
- [media_buffer.h](../src/common/media_buffer.h) - Buffer 管理封装
- [stream_dispatcher.h](../src/rkvideo/stream_dispatcher.h) - 流分发器
