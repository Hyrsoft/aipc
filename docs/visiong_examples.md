# VisionG 示例适配与 RV1106 验证

本文记录用户提供的 `示例.zip` 到 AIPC Lua 项目的完整映射、资源来源、板端结果和
已确认的 RV1106 约束。测试日期为 2026-08-08，板端 NPU 驱动为 0.9.2，AIPC 私有
RKNN runtime 为 2.3.2，硬件看门狗实际超时约 44 秒。

## 示例矩阵

| VisionG 文件 | AIPC 项目 | 板端结果 |
| --- | --- | --- |
| `test_rknn.py` | `yolov5-coco80` | 通过，约 10.1 FPS / 88.6 ms |
| `yolo11n.rknn` | `yolo11-coco80` | 通过，约 7.1 FPS / 128.1 ms |
| `number2.rknn` | `number-yolov5` | 通过，约 10.1 FPS / 31.8 ms |
| `yolo11n_number_320.rknn` | `yolo11-number-320` | 通过，约 10.0 FPS / 32.1 ms |
| `test_lprnet.py` | `lprnet` | 通过，约 5.0 FPS / 84.0 ms，输出文本对象 |
| `test_mlsd.py` / large | `mlsd` | 通过，约 7.7 FPS / 121.1 ms |
| `mlsd.zip` / tiny | `mlsd-tiny` | 通过，约 10.1 FPS / 49.0 ms |
| `test_ppocr.py` / v4 REC | `ppocr` | 通过，约 5.0 FPS / 83.6 ms |
| `ppocr.zip` / v6 REC | `ppocr-v6` | 通过，约 5.0 FPS / 80.9 ms |
| `test_nanotrack.py` | `nanotrack` | 通过，约 10.3 FPS / 59.8 ms，`kind=track` |
| `test_find_blob.py` | `find-blobs` | 通过，约 10 FPS；`hsv.calib` 未被原 Python 文件引用 |
| `test_ive_filter.py` | `ive-filter` | 通过；四种 5×5 kernel 每 5 秒轮换 |
| `test_ive_ncc.py` | `ive-ncc` | 通过，约 5 FPS / 13.8 ms，相似度约 0.90 |
| `test_npu_clock.py` | `npu-clock` | 通过，`aclk_npu_root=420000000`，保留人工确认 guard |
| `test_rtsp.py` | `visiong-rtsp` | 通过；daemon RTSP 为 H.264 1920×1080，`:8554/live` |
| `test_gui.py` | `visiong-webui-gui` | 通过；映射到 WebUI、preview 与录像 API |
| `test_display_spi.py` | `visiong-spi-display` | Lua 能力映射通过；板端当前无 `spidev` 节点，未做物理屏输出 |
| `test_isp_af.py` | `visiong-isp-af` | Lua 能力映射通过；ISP/AIQ 由 media_worker 独占 |
| `fb0.sh` | SDK 设备树配置 | 不在线执行；脚本会直接写启动介质 FDT/DTB |

## 两个已修复的系统兼容问题

Luckfox SDK 固件自带 `librknnmrt.so` 1.6.0。PPOCR 的识别模型使用 `exNorm`
fallback，旧 runtime 会以 `unsupport cpu exNorm op` 退出。AIPC 从 Rockchip 官方
`rknn-toolkit2` 固定提交下载 armhf-uclibc runtime 2.3.2，按 SHA-256 校验后放进
应用私有 `lib/`，不覆盖系统 `/oem/usr/lib`。官方文档建议驱动版本至少为 0.9.2，
与当前固件一致。

第二个问题是 RV1106 VPSS 对过小的在线 AI 通道不会可靠返回错误，而可能直接硬锁
SoC。实测 `320×240`、由 `320×320 contain` 产生的 `320×180`、以及
`640×240` 路径都可触发复位。框架现在要求 AI 传输通道宽至少 384、高至少 256；
320×320 RKNN 模型使用 640×640 NV12 通道，由 VisionG 内部完成最终模型缩放。

## 看门狗恢复验证

定位 VPSS 问题期间多次触发真实硬锁。每次 ADB、HTTP 和 RTSP 均先断开，硬件
看门狗随后使 boot ID 变化，`S99aipc` 自动启动 daemon，重新 armed `/dev/watchdog`，
并恢复最后一次成功的 AI 项目。整个过程不需要人工复位，也未修改任何分区。

一次依赖 factory 恢复实验触发复位后，ext4 将 rootfs 保护性挂载为只读，而
`/userdata` 保持可写。AIPC 因此将应用包、PID、日志、AI 状态、录像和依赖 active
override 统一迁移到 `/userdata/aipc-rust`，避免复位后的启动入口再次依赖 `/root` 写入。

## 资源获取

全部 13 个 RKNN 模型和 5 个辅助资源由 `scripts/fetch-ai-models.sh` 从固定的
GitHub Release `ai-models-v1.0.0` 下载。归档和每个文件均进行 SHA-256 校验，
也可通过 `AIPC_AI_MODELS_ARCHIVE` 指定本地归档完成离线构建；大模型不写入 Git
历史。RKNN runtime 由 `scripts/fetch-rknn-runtime.sh` 独立下载和校验。
