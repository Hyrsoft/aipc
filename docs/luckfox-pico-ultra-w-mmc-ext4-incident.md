# Luckfox Pico Ultra W 媒体重启伴随 eMMC/EXT4 故障记录

## 文档状态

- 板卡：Luckfox Pico Ultra W
- SoC：Rockchip RV1106
- 固件内核：`Linux 5.10.160 #1 Thu Aug 6 00:27:59 HKT 2026`
- 测试分支：`feat/audio-e2e`
- 记录日期：2026-08-06
- 当前结论：已确认文件系统异常的直接原因是 eMMC/MMC 控制器读写超时；媒体 worker 快速释放并重建是稳定触发场景之一，但硬件供电、信号完整性、控制器或内核驱动中的具体根因仍需进一步区分。

## 问题概述

在 Luckfox Pico Ultra W 上进行 AIPC 音视频并发、录像和 worker 恢复压力测试时，板卡曾出现以下异常：

- HTTP、RTSP、以太网、Wi-Fi 和 TCP ADB 同时失联；
- worker 重启后进入 `running` 阶段，但视频和音频持续为 0 帧；
- MMC 控制器出现 command/xfer timeout；
- 根分区发生块设备读写错误；
- EXT4 journal 中止，根文件系统被内核重新挂载为只读；
- 重启后持久化日志中出现零填充区域，与崩溃时未完成的数据写回相符。

USB 拓展坞增加外接供电后，常规存储和音视频并发测试可以通过，但 worker 故障恢复和重启仍能触发异常。因此，单纯增加拓展坞供电不能消除该问题。

## 基础状态

重新烧录固件后的初始检查结果：

- eMMC `life_time=0x01 0x01`；
- eMMC `pre_eol_info=0x01`；
- EXT4 初始状态为 `clean`；
- EXT4 `Errors behavior` 为 `Continue`；
- 启动时没有 MMC、块设备或 EXT4 错误。

上述寿命计数表明 eMMC 尚未接近磨损寿命终点，不能用闪存正常磨损解释本次故障。

## 已通过的压力测试

在故障触发前，下列测试均正常：

1. 64 MiB 文件使用 `fsync` 写入，回读 SHA-256 一致；
2. 创建并同步 500 个小文件；
3. WebSocket 预览持续 20 秒：
   - 601 个 H.264 视频帧；
   - 1000 个 G711A 音频帧；
4. RTSP TCP 和 UDP 并发拉流均成功；
5. 并发录像生成：
   - MP4 时长 `35.667056 s`；
   - WAV 时长 `35.673000 s`；
   - 音画时长相差约 `6 ms`；
   - MP4 和 WAV Range 请求均返回 HTTP 206；
6. 并发测试结束后，worker 的视频、音频 IPC 丢包和错误均为 0。

这说明常规连续读写和稳定运行负载不是必现条件，异常与媒体资源退出、重新初始化的关系更明显。

## 复现过程

### 强制终止 worker

在媒体管线正常运行时，对 `media_worker` 发送 `SIGKILL`：

1. HTTP 接口开始超时；
2. TCP ADB 变为 `offline`；
3. 板卡的以太网和 Wi-Fi 地址全部不可达；
4. 一分钟观察窗口内没有自行恢复。

该场景会绕过 worker 用户态的正常 RKMPI/ISP 释放流程，可能使内核在进程退出清理设备资源时进入异常状态。

### 正常停止 worker

通过 `POST /api/v1/worker/stop` 发送 SIGTERM：

1. worker 执行音频、视频、VI、VENC、RKMPI 和 ISP 的正常释放；
2. 约 1.2 秒后进入 `stopped`；
3. 板卡和网络保持可用；
4. 本次单独停止没有立即产生 MMC 错误。

这说明正常资源释放本身可以完成，`SIGKILL` 退出路径的风险明显更高。

### 正常 restart

重新启动 worker 并等待媒体正常后，通过 `POST /api/v1/worker/restart` 执行一次正常重启：

1. 旧 worker 正常关闭；
2. supervisor 在旧进程退出后立即创建下一代 worker；
3. 下一代 worker 完成 ISP、VI、VENC 和音频初始化，并报告 `running`；
4. 视频和音频始终没有数据，timeout 持续增加；
5. worker 报告 `video stream did not recover after consecutive timeouts`；
6. 后续释放阶段出现 `rkisp_stream_stop id:0 timeout`。

因此，即使不使用 `SIGKILL`，快速“释放后立即重建”也能触发媒体管线异常。

## 关键内核证据

旧媒体管线释放后，内核首先报告 MMC 命令和传输超时：

```text
dwmmc_rockchip ffaa0000.mmc: Unexpected command timeout, state 3
dwmmc_rockchip ffaa0000.mmc: Unexpected xfer timeout, state 3
```

随后 MMC 控制器尝试重新初始化总线：

```text
mmc_host mmc0: Bus speed (slot 0) = 400000Hz
mmc_host mmc0: Bus speed (slot 0) = 49500000Hz
mmc0: switch to bus width 8 failed
mmc0: switch to bus width 4 failed
```

根分区开始出现实际写错误：

```text
blk_update_request: I/O error, dev mmcblk0, sector 1640000 op 0x1:(WRITE)
Buffer I/O error on dev mmcblk0p7, logical block 0, lost async page write
Buffer I/O error on dev mmcblk0p7, logical block 1, lost async page write
EXT4-fs (mmcblk0p7): previous I/O error to superblock detected
```

之后出现大量读错误，并最终中止 journal：

```text
blk_update_request: I/O error, dev mmcblk0, sector 2115064 op 0x1:(WRITE)
Aborting journal on device mmcblk0p7-8.
EXT4-fs error (device mmcblk0p7): ext4_journal_check_start:83: Detected aborted journal
EXT4-fs (mmcblk0p7): Remounting filesystem read-only
```

文件系统损坏的直接链路因此可以确定为：

```text
MMC 控制器/总线超时
→ 块设备读写 I/O error
→ 文件数据和 EXT4 metadata 异步写回丢失
→ EXT4 journal 中止
→ 根文件系统被重新挂载为只读
```

文件系统损坏不只是板卡卡死后强制断电的次生结果；在本次复现中，板卡仍在线时已经观察到真实的 eMMC 读写失败。

## USB 链路现象

主机侧还持续出现以下 USB 错误：

```text
device descriptor read/64, error -71
Device not responding to setup address
device not accepting address, error -71
unable to enumerate USB device
```

在较早测试中还出现过 RNDIS `NETDEV WATCHDOG`。增加拓展坞外接供电后，USB `error -71` 仍然存在。

USB 枚举错误不能直接解释根分区的 MMC 写失败，但它说明当前硬件连接、供电或信号环境还存在独立的不稳定因素。后续验证不能只依赖拓展坞供电结果。

## 代码时序观察

当前 supervisor 的 restart 流程会先设置 `after_stop`，向旧 worker 发送 SIGTERM，并在收到旧进程退出消息后立即调用 `spawn_process` 启动下一代 worker。

该实现能够保证旧用户态进程已经退出，但没有为 ISP、CIF、VENC、音频设备和相关内核线程提供额外的冷却或稳定窗口。板端日志表明旧管线释放与下一代初始化之间只有很短的间隔。

这可能放大 RV1106 BSP 驱动或硬件时序问题，但应用层增加延迟只能作为规避和诊断手段，不能代替对 MMC I/O 错误的硬件与内核根因排查。

## 当前判断

按证据强度排序，可能原因如下：

1. Luckfox Pico Ultra W 板端 eMMC 供电、信号完整性或器件连接异常；
2. RV1106 MMC 控制器、时钟、复位或 BSP 驱动异常；
3. ISP/CIF/RKMPI 释放和重建过程中对共享电源、时钟、DMA 或总线资源产生干扰；
4. 板卡整体供电瞬态仍不稳定，拓展坞外接供电没有覆盖实际故障电源轨；
5. eMMC 器件间歇性故障，但目前寿命计数不支持正常磨损失效。

目前没有证据表明 AIPC 写入了错误扇区或绕过文件系统直接访问块设备。应用写入只是暴露了底层 MMC 传输失败。

## 风险与现场处置

一旦出现下列任一日志，应立即停止压力测试：

- `Unexpected command timeout`；
- `Unexpected xfer timeout`；
- `blk_update_request: I/O error`；
- `Buffer I/O error`；
- `Aborting journal`；
- `Remounting filesystem read-only`。

不要在已经只读或 journal aborted 的根文件系统上继续部署、录像或修改配置。应先断电并重新烧录，或在根分区未挂载的环境中执行离线检查。在线运行 `fsck` 不安全，也不能修复仍在发生的硬件传输错误。

## 后续验证计划

### 硬件与固件

1. 使用独立、稳定、满足峰值电流要求的电源直接为 Luckfox Pico Ultra W 供电，绕过拓展坞供电路径；
2. 更换 USB 线、拓展坞和主机端口，单独确认 USB `error -71` 是否消失；
3. 在另一块同型号板卡上运行同一固件和压力流程；
4. 使用厂商原始示例测试 ISP 重启和 eMMC 并发读写，排除 AIPC 特有逻辑；
5. 对比不同 BSP/内核版本的 MMC、RKISP、RKCIF 和 Rockit 驱动；
6. 如条件允许，测量媒体管线启停期间 eMMC 和 SoC 相关电源轨的压降与纹波。

### 软件诊断

在文件系统恢复且存储基线再次通过后，按以下顺序测试：

1. 正常停止 worker，等待 10 秒后启动；
2. 将等待时间依次缩短为 5 秒、2 秒和 1 秒；
3. 每轮同时采集 `/proc/kmsg`、supervisor events 和 worker stderr；
4. 每次启停前后检查 MMC、EXT4、RKISP 和 RKCIF 日志；
5. 暂时禁止自动 `SIGKILL` 恢复，避免在未正常释放设备资源时立即重建；
6. 为 supervisor 增加可配置的 restart cooldown，验证其是否能规避媒体 0 帧和 MMC timeout；
7. 即使 cooldown 有效，也要继续确认底层为何会发生 MMC I/O error，不能将延迟视为最终修复。

## 结论

Luckfox Pico Ultra W 上的文件系统损坏已经由板端日志证实是 eMMC/MMC 传输错误直接导致。媒体 worker 快速重启是明确的触发场景，强制终止 worker 的风险更高；拓展坞外接供电没有消除问题。

在确定板端供电、eMMC 信号和 RV1106 BSP 驱动稳定之前，不应把自动快速重启作为生产环境中的可靠恢复机制。
