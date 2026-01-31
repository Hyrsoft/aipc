# AIPC 项目代码规范

本项目遵循 [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)，并根据嵌入式开发的特点进行了部分调整。

## 目录
- [命名规范](#命名规范)
- [文件规范](#文件规范)
- [格式规范](#格式规范)
- [注释规范](#注释规范)
- [最佳实践](#最佳实践)

---

## 命名规范

### 文件名
- 使用 **全小写 + 下划线** 命名
- 头文件使用 `.h` 后缀，源文件使用 `.cpp` 后缀

```
✅ media_buffer.h
✅ stream_dispatcher.cpp
❌ MediaBuffer.h
❌ streamDispatcher.cpp
```

### 类型名（类、结构体、枚举、类型别名）
- 使用 **PascalCase**（大驼峰命名法）

```cpp
// ✅ 正确
class StreamDispatcher;
struct VideoConfig;
enum class StreamType;
using VideoFramePtr = std::shared_ptr<VIDEO_FRAME_INFO_S>;

// ❌ 错误
class stream_dispatcher;
struct video_config;
```

### 函数名
- **普通函数和类的公有方法**：使用 **PascalCase**
- **私有/内部辅助函数**：也使用 **PascalCase**（与 Google 风格一致）

```cpp
// ✅ 正确 - 类方法
class StreamDispatcher {
public:
    void RegisterConsumer(const std::string& name);
    void Start();
    void Stop();
    bool IsRunning() const;
    
private:
    void FetchLoop();
    void DispatchLoop();
};

// ✅ 正确 - 自由函数（与 SDK 风格保持一致，使用 snake_case）
VideoFramePtr acquire_video_frame(RK_S32 dev_id, RK_S32 chn_id);
void* get_frame_vir_addr(const VideoFramePtr& frame);
```

> **注意**：为了与 RKMPI SDK 的 C 风格 API 保持一致，底层封装函数（如 `acquire_video_frame`）使用 `snake_case`。

### 变量名
- **局部变量和函数参数**：使用 **snake_case**
- **类成员变量**：使用 **snake_case_**（带尾部下划线）
- **全局变量**：使用 **g_** 前缀 + **snake_case**

```cpp
// ✅ 正确
void ProcessFrame(int frame_width, int frame_height) {
    int buffer_size = frame_width * frame_height;
}

class StreamDispatcher {
private:
    RK_S32 venc_chn_id_;          // 成员变量带尾部下划线
    std::atomic<bool> running_;
    std::thread fetch_thread_;
};

// 全局变量
static VideoConfig g_video_config;
static std::unique_ptr<StreamDispatcher> g_dispatcher;
```

### 常量
- **编译时常量**：使用 **k** 前缀 + **PascalCase**
- **宏常量**：使用 **全大写 + 下划线**

```cpp
// ✅ 正确
const int kDefaultBufferSize = 1024;
constexpr int kMaxFrameRate = 60;

#define LOG_TAG "rkvideo"
#define MAX_QUEUE_SIZE 10
```

### 枚举值
- 使用 **k** 前缀 + **PascalCase**
- 优先使用 `enum class`

```cpp
// ✅ 正确
enum class StreamType {
    kRtsp,
    kWebRtc,
    kRecord
};

// ❌ 避免
enum StreamType {
    STREAM_TYPE_RTSP,
    STREAM_TYPE_WEBRTC
};
```

---

## 文件规范

### 头文件保护
- 使用 `#pragma once`（编译器广泛支持，更简洁）

```cpp
// ✅ 推荐
#pragma once

// 也可以使用传统方式
#ifndef AIPC_COMMON_MEDIA_BUFFER_H_
#define AIPC_COMMON_MEDIA_BUFFER_H_
// ...
#endif  // AIPC_COMMON_MEDIA_BUFFER_H_
```

### include 顺序
按以下顺序排列，每组之间空一行：
1. 相关头文件（对应的 .h 文件）
2. C 系统头文件
3. C++ 标准库头文件
4. 第三方库头文件
5. 本项目头文件

```cpp
// example.cpp
#include "example.h"

#include <cstring>

#include <memory>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "common/logger.h"
#include "common/media_buffer.h"
```

---

## 格式规范

### 缩进
- 使用 **4 个空格**，不使用 Tab

### 行宽
- 每行最多 **100** 个字符

### 大括号
- 使用 **K&R 风格**（左大括号不换行）

```cpp
// ✅ 正确
if (condition) {
    DoSomething();
} else {
    DoSomethingElse();
}

void Function() {
    // ...
}

class MyClass {
public:
    void Method();
};
```

### 指针和引用
- `*` 和 `&` 靠近类型名

```cpp
// ✅ 正确
int* ptr;
const std::string& name;

// ❌ 错误
int *ptr;
const std::string &name;
```

### 命名空间
- 不缩进命名空间内容
- 结尾注释标明命名空间

```cpp
namespace aipc {
namespace video {

class Processor {
    // ...
};

}  // namespace video
}  // namespace aipc
```

---

## 注释规范

### 文件头注释
使用 Doxygen 风格

```cpp
/**
 * @file media_buffer.h
 * @brief 媒体资源管理 - 基于 RAII 的 RKMPI 资源封装
 *
 * 详细描述...
 *
 * @author 作者名
 * @date 2026-01-31
 */
```

### 函数注释

```cpp
/**
 * @brief 从 VI 通道获取原始帧并包装为智能指针
 * 
 * @param dev_id VI 设备 ID
 * @param chn_id VI 通道 ID
 * @param timeout_ms 超时时间（毫秒），-1 表示阻塞等待
 * @return VideoFramePtr 成功返回帧指针，失败返回 nullptr
 * 
 * @note 返回的智能指针在所有引用释放后会自动调用 RK_MPI_VI_ReleaseChnFrame
 */
VideoFramePtr acquire_video_frame(RK_S32 dev_id, RK_S32 chn_id, RK_S32 timeout_ms = -1);
```

### 行内注释
- 使用 `//` 进行行内注释
- 注释与代码之间空两格

```cpp
int count = 0;  // 初始化计数器
```

### TODO 注释

```cpp
// TODO(username): 添加错误处理逻辑
// FIXME(username): 修复内存泄漏问题
```

---

## 最佳实践

### 智能指针
- 优先使用 `std::unique_ptr`，需要共享所有权时使用 `std::shared_ptr`
- 避免裸指针拥有资源

```cpp
// ✅ 正确
auto dispatcher = std::make_unique<StreamDispatcher>();
VideoFramePtr frame = acquire_video_frame(0, 0);

// ❌ 避免
StreamDispatcher* dispatcher = new StreamDispatcher();
```

### RAII
- 资源获取即初始化
- 利用析构函数自动释放资源

```cpp
// 使用自定义删除器的 shared_ptr 管理 MPI 资源
return VideoFramePtr(frame, [dev_id, chn_id](VIDEO_FRAME_INFO_S* p) {
    if (p) {
        RK_MPI_VI_ReleaseChnFrame(dev_id, chn_id, p);
        delete p;
    }
});
```

### 错误处理
- 使用返回值或异常处理错误
- 关键操作使用日志记录

```cpp
RK_S32 ret = RK_MPI_VI_GetChnFrame(dev_id, chn_id, frame, timeout_ms);
if (ret != RK_SUCCESS) {
    LOG_ERROR("Failed to get VI frame, error: {:#x}", ret);
    return nullptr;
}
```

### 线程安全
- 明确标注线程安全性
- 使用 `std::mutex` 保护共享数据
- 优先使用 `std::lock_guard` 或 `std::unique_lock`

```cpp
std::lock_guard<std::mutex> lock(mutex_);
queue_.push(std::move(item));
```

### 日志使用
- 使用项目提供的日志宏
- 在文件开头定义 `LOG_TAG`

```cpp
#define LOG_TAG "rkvideo"
#include "common/logger.h"

void SomeFunction() {
    LOG_INFO("Initializing...");
    LOG_DEBUG("Debug info: {}", value);
    LOG_ERROR("Error occurred: {}", error_msg);
}
```

---

## 工具配置

### clang-format
项目根目录的 `.clang-format` 文件配置了代码格式化规则，可使用以下命令格式化代码：

```bash
clang-format -i src/**/*.cpp src/**/*.h
```

### 编辑器配置
推荐在 VS Code 中安装以下扩展：
- C/C++ (Microsoft)
- clang-format
- Doxygen Documentation Generator

---

## 参考资料
- [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
- [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
