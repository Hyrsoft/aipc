# RV1106 硬件看门狗

本文说明 Luckfox Pico Ultra（RV1106）硬件看门狗的 SDK 配置、AIPC
喂狗策略、Docker 全量镜像构建、烧录后验证和真实复位实验流程。

## 1. 问题与保护范围

RV1106 SDK 已包含 Synopsys DesignWare Watchdog 驱动：

```text
CONFIG_WATCHDOG=y
CONFIG_DW_WATCHDOG=y
```

但 Luckfox Pico Ultra 的默认设备树关闭了 WDT 节点，所以旧镜像中没有
`/dev/watchdog` 和 `/sys/class/watchdog/watchdog0`。仅运行 BusyBox 的
`watchdog` 命令也不能可靠判断 AIPC daemon 是否失去运行能力。

当前方案由 AIPC daemon 直接持有 `/dev/watchdog`：

- HTTP listener 初始化完成后启动硬件看门狗；
- 请求 30 秒超时，每 5 秒发送一次 `WDIOC_KEEPALIVE`；
- daemon 退出、运行时无法继续调度、内核卡死或喂狗失败后停止喂狗；
- 硬件计数器到期后复位 RV1106；
- `CONFIG_WATCHDOG_NOWAYOUT=y` 防止打开设备后被意外关闭。

保护从 AIPC daemon 就绪后开始。当前配置不覆盖 U-Boot 或 daemon 启动前的
早期启动卡死；如需覆盖该阶段，需要另外设计 U-Boot 到 Linux 的看门狗交接。

## 2. SDK 配置

SDK 位于 AIPC 仓库的上级目录。Luckfox Pico Ultra 使用以下板级配置：

```text
project/cfg/BoardConfig_IPC/
  BoardConfig-EMMC-Buildroot-RV1106_Luckfox_Pico_Ultra-IPC.mk
```

### 2.1 启用设备树节点

文件：

```text
sysdrv/source/kernel/arch/arm/boot/dts/rv1106g-luckfox-pico-ultra.dts
```

配置：

```dts
/**********WATCHDOG**********/
&wdt {
	status = "okay";
};
```

该节点来自 RV1106 公共设备树：

```dts
watchdog@ff5a0000 {
	compatible = "rockchip,rv1106-wdt", "snps,dw-wdt";
};
```

### 2.2 启用内核策略

文件：

```text
sysdrv/source/kernel/arch/arm/configs/luckfox_rv1106_linux_defconfig
```

配置：

```text
CONFIG_WATCHDOG=y
CONFIG_WATCHDOG_NOWAYOUT=y
CONFIG_WATCHDOG_SYSFS=y
CONFIG_DW_WATCHDOG=y
```

`NOWAYOUT` 表示硬件看门狗一旦被 AIPC 打开就不能通过普通 close 停止。正常部署
或 daemon 重启必须在超时窗口内完成，否则板子会复位。

## 3. AIPC 配置与实现

示例配置位于 `config/aipc-daemon.example.json`：

```json
"watchdog": {
  "enabled": true,
  "required": false,
  "device": "/dev/watchdog",
  "timeout_sec": 30,
  "feed_interval_ms": 5000
}
```

字段说明：

| 字段 | 含义 |
| --- | --- |
| `enabled` | 是否启用 AIPC 硬件看门狗 |
| `required` | 设备不存在时是否拒绝启动 daemon |
| `device` | Linux watchdog 字符设备路径 |
| `timeout_sec` | 请求的复位超时，驱动可能向上取整到硬件支持值 |
| `feed_interval_ms` | 喂狗间隔，校验要求小于超时的一半 |

兼容旧镜像时保持 `required=false`，daemon 会记录 warning 后继续运行。新的
watchdog 内核烧录并确认 `/dev/watchdog` 存在后，建议在板端配置中改为
`required=true`，这样错误镜像或驱动加载失败不会被静默忽略。

实现位于：

```text
aipc-daemon/src/watchdog.rs
aipc-daemon/src/config.rs
aipc-daemon/src/main.rs
```

使用的 Linux watchdog ioctl：

```text
WDIOC_KEEPALIVE = _IOR('W', 5, int)
WDIOC_SETTIMEOUT = _IOWR('W', 6, int)
```

daemon 不发送 magic close。喂狗任务发生错误时直接停止喂狗，让硬件执行复位。

## 4. Docker 全量编译 SDK 镜像

SDK 的 `docker/Dockerfile` 基于官方镜像：

```text
luckfoxtech/luckfox_pico:1.0
```

`docker/docker-compose.yml` 将 SDK 根目录挂载为容器内的 `/project`。从 SDK
根目录执行：

```bash
cd docker
docker compose up -d --build rv1106-dev
```

确认 `.BoardConfig.mk` 指向 Ultra EMMC Buildroot 配置：

```bash
docker exec luckfox-pico-sdk bash -lc \
  'readlink -f /project/.BoardConfig.mk'
```

预期末尾为：

```text
BoardConfig-EMMC-Buildroot-RV1106_Luckfox_Pico_Ultra-IPC.mk
```

然后在容器中运行 SDK 顶层 `build.sh` 的完整构建与保存目标：

```bash
docker exec luckfox-pico-sdk bash -lc \
  'cd /project && ./build.sh allsave'
```

`allsave` 会构建 U-Boot、kernel、rootfs、media、app，打包各分区镜像和
`update.img`，并保存 release/debug 辅助产物。这里不执行 `build.sh clean all`，
因为 SDK 的 clean-all 会删除 Buildroot 解包树、现有输出和配置链接；需要完全
无缓存重建时应先单独备份用户产物并明确执行。

主要烧录产物位于：

```text
output/image/
```

典型文件包括：

```text
boot.img
uboot.img
rootfs.img
oem.img
userdata.img
env.img
update.img
```

完整保存目录位于 SDK 根目录的 `IMAGE/` 下，具体目录名包含板型、启动介质和
构建时间。

### 4.1 构建后离线检查

确认最终 kernel config：

```bash
grep -E '^CONFIG_(WATCHDOG|WATCHDOG_NOWAYOUT|WATCHDOG_SYSFS|DW_WATCHDOG)=' \
  sysdrv/source/objs_kernel/.config
```

在 SDK Docker 容器中反编译最终 DTB：

```bash
docker exec luckfox-pico-sdk bash -lc \
  "dtc -I dtb -O dts \
  /project/sysdrv/source/objs_kernel/arch/arm/boot/dts/rv1106g-luckfox-pico-ultra.dtb \
  2>/dev/null | sed -n '/watchdog@ff5a0000 {/,/};/p'"
```

预期包含：

```text
status = "okay";
```

烧录前建议保存镜像校验值：

```bash
sha256sum output/image/*.img > output/image/SHA256SUMS
```

## 5. AIPC 部署约束

启用 `NOWAYOUT` 后，停止 daemon 会启动不可取消的硬件倒计时。AIPC 的 ADB
部署脚本因此采用以下顺序：

1. daemon 继续喂狗时，将新包上传到 `/root/aipc-rust.new`；
2. 上传成功后停止旧 daemon；
3. 原子切换当前目录和 previous 目录；
4. 复制持久状态，安装 `/etc/init.d/S99aipc` 开机启动链接；
5. 立即启动新 daemon。

这样上传 35 MB 左右的软件包不会消耗 30 秒的 watchdog 重启窗口。
watchdog 触发复位后，Buildroot init 会通过 `S99aipc` 自动恢复 AIPC，daemon
再次打开 `/dev/watchdog` 并重新进入保护状态。

## 6. 烧录后板端验证

完整 `update.img` 会重写 rootfs、OEM 和 userdata；当前 SDK 固件打包流程不会把
AIPC 的 `/root/aipc-rust` 部署目录嵌入镜像。烧录完成并重新进入 Linux 后，先
进行只读检查：

```bash
adb shell 'ls -l /dev/watchdog*'
adb shell 'ls -l /sys/class/watchdog/watchdog0'
adb shell 'cat /sys/class/watchdog/watchdog0/identity'
adb shell 'cat /sys/class/watchdog/watchdog0/timeout'
adb shell 'cat /sys/class/watchdog/watchdog0/nowayout'
adb shell 'dmesg | grep -i watchdog'
```

然后从 AIPC 仓库重新构建和部署用户态包：

```bash
./scripts/package-rv1106.sh
AIPC_ADB_SERIAL=BOARD_SERIAL ./scripts/deploy-rv1106-adb.sh
```

启动 AIPC 后检查：

```bash
adb shell 'grep -i watchdog /root/aipc-rust/data/daemon.stderr.log | tail -n 20'
curl -fsS http://BOARD_IP:8080/healthz
curl -fsS http://BOARD_IP:8080/api/v1/status
```

预期日志：

```text
hardware watchdog armed
```

日志会同时包含请求超时、驱动返回的实际超时和喂狗间隔。

## 7. 真实自动复位实验

真实复位实验会故意停止喂狗，应在镜像已经验证可启动、ADB/串口恢复路径可用且
没有分区写入操作时执行：

```bash
adb shell '/root/aipc-rust/scripts/stop.sh'
```

随后不要手动喂狗，预期板子在驱动返回的实际 timeout 附近自动复位。复位后验证：

```bash
adb wait-for-device
adb shell 'uptime; dmesg | grep -i watchdog'
curl -fsS http://BOARD_IP:8080/healthz
```

不要在烧录、文件系统扩容或重要数据写入过程中测试 watchdog reset。

## 8. 2026-08-08 Docker 构建记录

构建命令：

```bash
docker exec -i luckfox-pico-sdk bash -lc \
  'cd /project && ./build.sh allsave'
```

结果：U-Boot、kernel、Buildroot、media、app、分区镜像和 `update.img` 均构建
成功。保存目录：

```text
IMAGE/IPC_EMMC_BUILDROOT_RV1106_LUCKFOX_PICO_ULTRA_20260808.1706_RELEASE_TEST
```

关键校验值：

```text
boot.img   3b34e947a8e2f3208ae567bd72fa8f7c38de399b9bf39230e8ada1f6765a0ba1
update.img 060d0438ed904c19dfacf970d7be7bd0bcaae0d2c2df7b1a075198b8187dc00f
```

最终 `.config` 包含 `WATCHDOG_NOWAYOUT`、`WATCHDOG_SYSFS` 和
`DW_WATCHDOG`；最终 DTB 的 `watchdog@ff5a0000` 为 `status = "okay"`。

## 9. 2026-08-08 板端复位验证记录

烧录新固件后的只读检查结果：

```text
identity=Synopsys DesignWare Watchdog
requested timeout=30 seconds
actual timeout=44 seconds
nowayout=1
state=active
```

DesignWare 驱动会将请求值量化到硬件支持的 TOP 档位，因此本机实际超时为
44 秒。daemon 日志会记录 `requested_timeout_sec=30` 和
`actual_timeout_sec=44`，复位实验应以实际值为准。

真实实验步骤为停止 `/root/aipc-rust/scripts/stop.sh` 后不进行任何人工操作。
观测结果：

```text
停止测试开始后 53 秒：ADB 断开
停止测试开始后 62 秒：ADB 重新连接
复位前 boot ID：4b22e592-9f11-4931-8cba-35ac624b55ac
复位后 boot ID：d42900eb-8b10-41c0-a72a-2af524b47f58
```

复位后 `/etc/init.d/S99aipc` 自动启动 AIPC，daemon 再次记录
`hardware watchdog armed`，watchdog 回到 `state=active`。该平台本次复位后的
`bootstatus` 仍为 `0`，因此不要依赖 bootstatus 判断复位原因，应结合 boot ID、
ADB 断连时间和串口/启动日志判断。

恢复后的业务检查同时通过：

- HTTP health、视频/音频 preview 正常；
- RTSP 为 H.264 High Profile、1920×1080、30 FPS；
- `media-pipeline` Lua 示例成功产生首个推理结果；
- `yolov5-coco80` 运行约 9.4 FPS，平均推理约 86.9 ms；
- 5.47 秒 MP4 录像完成，HTTP Range 返回 206，`ffprobe` 可正常解析；
- AI、录像和 RTSP 负载下 watchdog 保持 `state=active`。
