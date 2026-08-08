# VisionG 示例到 AIPC/Lua 的适配

这些项目运行在独立的 `ai_worker` 进程中。摄像头帧由 `media_worker` 以 AIPF
传入，Lua 只负责算法编排，不能直接打开设备、网络、文件或动态库。所有项目都
使用 `aipc.infer(frame, model, options)`；旧的 `aipc.detect` 仍然兼容。

| Python 示例 | AIPC 项目 | 说明 |
| --- | --- | --- |
| `test_rknn.py` | `yolov5-coco80`, `yolo11-coco80`, `number-yolov5` | NPU 检测结果转成统一对象数组 |
| `test_rknn.py`（YOLO11 数字模型） | `yolo11-number-320` | 8 类 YOLO11 数字模型，保留独立模型资源 |
| `test_lprnet.py` | `lprnet` | 直接整帧车牌识别，文本放在 `label`/`text` |
| `test_mlsd.py` | `mlsd` | 线段转成带 `kind=line` 的结构化结果 |
| `test_mlsd.py`（tiny 模型） | `mlsd-tiny` | 320×320 tiny 模型，用同一结构化线段接口 |
| `test_ppocr.py` | `ppocr` | DET/REC/字典作为 `files` 资源，OCR 四边形保留在 `quad` |
| `test_ppocr.py`（v6 REC） | `ppocr-v6` | 复用 v3 DET，切换 v6 REC 与 v6 字典 |
| `test_nanotrack.py` | `nanotrack` | Lua 保存选择框、平滑位置并在丢失后回中心重启 |
| `test_find_blob.py` | `find-blobs` | HSV 阈值转成 blob 对象 |
| `test_ive_filter.py` | `ive-filter` | 在 AI 进程执行硬件滤波；NV12 先转 GRAY8，输出为空表示只做处理/健康检查 |
| `test_ive_ncc.py` | `ive-ncc` | 以模型目录中的模板图片计算 NCC 相似度 |
| `test_npu_clock.py` | `npu-clock` | 对齐 Python 参数设置 420 MHz、更新 CRU 并 rebind NPU，默认由 runtime guard 阻止在线部署 |
| `test_rtsp.py` | `visiong-rtsp` | RTSP 由 AIPC daemon 提供，Lua 发布 daemon-managed capability 结果 |
| `test_gui.py` | `visiong-webui-gui` | GUI/相册映射到 WebUI、preview 与录像 API，不抢 framebuffer |
| `test_display_spi.py` | `visiong-spi-display` | SPI 显示标记为外部 media sink，Lua 只验证输入帧与能力边界 |
| `test_isp_af.py` | `visiong-isp-af` | ISP/AF 归 media_worker 所有，不向不可信 Lua 暴露 AIQ 指针 |

模型由 `scripts/fetch-ai-models.sh` 下载并按 SHA-256 校验。`ive-ncc` 的小型模板
资源来自用户示例包；没有该文件时可以上传任意 JPEG 到 `/api/v1/ai/models`，并在
manifest 的 `model` 字段引用它。

RV1106 包会通过 `scripts/fetch-rknn-runtime.sh` 固定并校验官方 RKNN runtime
2.3.2。Luckfox SDK 自带的 1.6.0 runtime 不支持 PPOCR 模型使用的 `exNorm`
fallback；2.3.2 与当前 0.9.2 NPU 驱动兼容，并作为 AIPC 私有库加载，不覆盖板端
`/oem/usr/lib/librknnmrt.so`。

## 板端验证结果（2026-08-08）

- `yolov5-coco80`：通过，约 10.1 FPS / 88.6 ms。
- `yolo11-coco80`：通过，约 7.1 FPS / 128.1 ms。
- `number-yolov5`：通过，约 10.1 FPS / 31.8 ms。
- `yolo11-number-320`：通过，约 10.0 FPS / 32.1 ms。
- `lprnet`：通过，约 5.0 FPS / 84.0 ms，输出结构化车牌文本。
- `mlsd`：通过，约 7.7 FPS / 121.1 ms。
- `mlsd-tiny`：通过，约 10.1 FPS / 49.0 ms。
- `media-pipeline`：通过，约 2 FPS / 0.1 ms，媒体链路保持健康。
- `find-blobs`：通过，约 10 FPS / 12 ms。
- `ive-filter`：通过（NV12→GRAY8），约 5 FPS / 9–12 ms；Lua 每 5 秒轮换 Gaussian、Sharpen、Edge、Emboss 四个 Python 样例 kernel。
- `ive-ncc`：通过，约 5 FPS / 15.6 ms，相似度约 0.89。
- `ppocr`：通过，约 5.0 FPS / 83.6 ms。
- `ppocr-v6`：通过，约 5.0 FPS / 80.9 ms。
- `nanotrack`：通过，约 10.3 FPS / 59.8 ms，输出 `kind=track`。
- `npu-clock`：通过，已设置并读取到 `aclk_npu_root=420000000`；因会修改硬件时钟，仍保留 runtime guard。

`yolo11-number-320`、`ppocr-v6` 和 `nanotrack` 等资源来自用户提供的
VisionG 示例包。模型不进入 Git；设置 `AIPC_VISIONG_SAMPLE_ARCHIVE` 后，
`scripts/fetch-ai-models.sh` 会解包、校验并加入本地打包目录。没有该变量时，
可用示例仍会正常构建，缺失资源的项目会在部署校验阶段明确报错。

附件中的 `fb0.sh` 会直接读写启动介质内的 FDT header 和 DTB，不属于运行期
VisionG API，也不适合放进可在线部署的 Lua 沙箱。AIPC 只记录其对应关系，实际
屏幕反色应在 SDK 设备树中配置并随固件构建，不能由 AI 项目修改分区。

板端复位最终定位为过小的实时 AI VPSS 通道，而非数字模型或 LPRNet 本身。
RV1106 当前要求传输通道至少为 384×256；数字模型使用安全的 640×640 通道，
由 VisionG 在 NPU 内部继续缩放到模型的 320×320。daemon 与 media_worker 已同时
加入该约束，避免驱动硬锁。只有具有真实硬件副作用的 `npu-clock` 继续使用
`options.runtime_guard`。
