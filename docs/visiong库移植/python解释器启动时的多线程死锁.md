我先确认一下 `WarmupVisionGPythonRuntime` 和 `PythonRuntime` 的调用时机，判断是不是 `std::call_once` 被其他线程占住导致你看到“只打印 ctor begin”。然后给你一个可直接验证的最小排查步骤。

搜索正则表达式 `WarmupVisionGPythonRuntime|CreateVisionGProducer|std::make_shared<PythonRuntime>|EnsureEmbeddedPythonReady` (`**/src/**`)，12 个结果

搜索正则表达式 `SwitchMode|Init\(|VisionGProducer|Warmup` (`**/src/media_producer/**`)，20 个结果

读取 [](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/visiong/visiong_producer.h)

读取 [](file:///home/hao/projects/luckfox-pico/aipc/src/main.cpp#90-90)，行 90 到 150

我已经确认 `main` 启动时会先预热一次解释器，所以你在构造函数里看不到 `EnsureEmbeddedPythonReady` 内日志是正常的。下一步我再看 `SwitchMode/Init/Stop` 的并发流程，判断你卡在 `gil_scoped_acquire` 的真正锁等待点。

读取 [](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/media_manager.cpp#1-1)，行 1 到 280

读取 [](file:///home/hao/projects/luckfox-pico/aipc/src/media_producer/visiong/visiong_producer.cpp#214-214)，行 214 到 436

你这个现象已经不是之前的 SIGILL 问题了，当前更像是“卡住/死锁”，而且点位很典型。

先说你问的“为什么 EnsureEmbeddedPythonReady 里的日志不打印”：
1. 这个函数用了 std::call_once，只会真正执行一次 lambda。
2. 你在主线程启动早期已经预热过一次，见 main.cpp。
3. 所以后面在 PythonRuntime 构造里再次调用时，通常不会再进入 lambda，自然不会再打印其中日志，见 visiong_producer.cpp 和 visiong_producer.cpp。

你现在真正卡住的位置：
1. GDB 显示线程停在 pybind11::gil_scoped_acquire，说明在等 GIL，不是崩溃。
2. 你贴的日志里 exec-finish 等很久后手动中断，主线程在 pthread_cond_wait，这也符合“某线程在等锁”。

高概率根因是 PythonRuntime 里锁顺序不一致，存在死锁风险：
1. LoadCode 是 先拿 mutex_ 再拿 GIL，见 visiong_producer.cpp。
2. Shutdown 也是 先拿 mutex_ 再拿 GIL，见 visiong_producer.cpp。
3. 但 CallRun 是 先拿 GIL 再拿 mutex_，见 visiong_producer.cpp。
4. 这三者并发时会形成经典循环等待。

所以你的“只看到 ctor begin，后面没日志”是合理现象：
1. EnsureEmbeddedPythonReady 可能已经被 call_once 跳过。
2. 随后 ctor 里的 gil_scoped_acquire 卡住，见 visiong_producer.cpp。

建议的修复方向：
1. 统一锁顺序，所有涉及 Python API 的路径都改成先拿 GIL，再拿 mutex_，避免反向顺序。
2. 或者更严格：不在持有 mutex_ 时调用任何 Python API（包括 py::exec、cleanup_fn_、init_fn_）。

你可以先用这组调试命令验证死锁链路（卡住时执行）：
1. -exec info threads
2. -exec thread apply all bt
3. 看是否一条线程在 CallRun 路径持有 GIL 等 mutex，另一条在线程在 LoadCode 或 Shutdown 持有 mutex 等 GIL。

如果你愿意，我下一步可以直接给你一版最小改动的重构方案（只改 PythonRuntime 三个方法的加锁顺序），并列出每一处改动点和风险。