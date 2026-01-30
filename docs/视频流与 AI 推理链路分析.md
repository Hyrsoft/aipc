# Luckfox Pico RK_MPI 视频流与 AI 推理链路分析

主要分析 [`luckfox_pico_rtsp_retinaface_osd` 例程](https://github.com/LuckfoxTECH/luckfox_pico_rkmpi_example/blob/kernel-5.10.160/example/luckfox_pico_rtsp_retinaface_osd/src/main.cc)，探讨其如何实现摄像头采集、NPU 推理、硬件编码及 RTSP 推流的完整链路。

------

## 1. 视频流链路概览

该程序在硬件层面上构建了两条并行的核心路径，通过 `RK_MPI`（Rockchip Media Process Interface）协调不同硬件模块（VI、VENC、NPU、RGN）的交互。

| **路径名称** | **数据流向**                                    | **核心功能**                                  |
| ------------ | ----------------------------------------------- | --------------------------------------------- |
| **推流路径** | VI Chn0 $\rightarrow$ VENC $\rightarrow$ RTSP   | 负责视频采集、硬件 H.264 编码及网络传输       |
| **推理路径** | VI Chn1 $\rightarrow$ CPU/NPU $\rightarrow$ RGN | 负责获取原始帧、图像预处理、AI 推理及结果绘制 |

------

## 2. VI 模块初始化与格式选择

程序初始化了两个 VI 通道，分别进行推流与AI推理。

```cpp
	// vi init
	vi_dev_init();
	vi_chn_init(0, width, height);
	vi_chn_init(1, width, height);
```

### 2.1 通道分配

- **通道 0**：负责推流，直接绑定至编码器。
- **通道 1**：负责 AI 推理，供用户态程序主动获取帧数据。

### 2.2 像素格式与配置

两个通道均设置为 `RK_FMT_YUV420SP`（即 **NV12** 格式）。采用 `VI_V4L2_MEMORY_TYPE_DMABUF` 以减少内存拷贝开销。

```cpp
int vi_chn_init(int channelId, int width, int height) {
  int ret;
  int buf_cnt = 4;
  // VI init
  VI_CHN_ATTR_S vi_chn_attr;
  memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
  vi_chn_attr.stIspOpt.u32BufCount = buf_cnt;
  vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF; 
  vi_chn_attr.stSize.u32Width = width;
  vi_chn_attr.stSize.u32Height = height;
  vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
  vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE; 
  vi_chn_attr.u32Depth = 2; 
  ret = RK_MPI_VI_SetChnAttr(0, channelId, &vi_chn_attr);
  ret |= RK_MPI_VI_EnableChn(0, channelId);
  if (ret) {
    printf("ERROR: create VI error! ret=%d\n", ret);
    return ret;
  }
  return ret;
}
```

------

## 3. 推流通路 (VI Chn0 $\rightarrow$ VENC $\rightarrow$ RTSP)

推流路径实现了**纯硬件链路**，数据在底层驱动中通过 `SYS_Bind` 自动流转。

### 3.1 硬件绑定逻辑

通过 `RK_MPI_SYS_Bind` 将 VI 模块与 VENC 模块在内核层关联，避免了将原始 YUV 数据频繁拷贝到用户空间。

```cpp
// bind vi to venc  
stSrcChn.enModId = RK_ID_VI;
stSrcChn.s32DevId = 0;
stSrcChn.s32ChnId = 0;
    
stvencChn.enModId = RK_ID_VENC;
stvencChn.s32DevId = 0;
stvencChn.s32ChnId = 0;

s32Ret = RK_MPI_SYS_Bind(&stSrcChn, &stvencChn);
```

### 3.2 编码与传输

1. **硬件编码**：VENC 接收到 NV12 数据，根据 `RK_VIDEO_ID_AVC` 配置进行 H.264 硬件压缩。
2. **流获取**：`GetMediaBuffer` 线程通过 `RK_MPI_VENC_GetStream(0, &stFrame, -1)` **阻塞式**获取码流。
3. **推流**：调用 `rtsp_tx_video` 将 H.264 数据包发送至客户端。

------

## 4. AI 推理路径 (VI Chn1 $\rightarrow$ NPU)

推理路径涉及用户态的图像处理与 NPU 调度。

### 4.1 图像获取与预处理

- **获取数据**：`RetinaProcessBuffer` 线程通过 `RK_MPI_VI_GetChnFrame` 从通道 1 获取 NV12 帧。
- **格式转换**：利用 `opencv-mobile` 将 **NV12** 转换为 **BGR**。
- **尺寸缩放**：调用 `cv::resize` 将图像适配模型输入的 $640 \times 640$ 尺寸。

### 4.2 NPU 任务调度

`inference_retinaface_model` 函数执行**同步推理**：CPU 调用后进入等待状态，直到 NPU 硬件完成计算并返回 `od_results` 结果。

------

## 5. OSD 叠加机制

OSD（On-Screen Display）并未修改原始图像内存，而是通过 **RGN（Region）模块** 实现。

- **原理**：AI 推理得到的坐标结果被转化为 RGN 画布操作。
- **硬件合成**：RGN 模块将线框作为叠加层，在进入 VENC 编码前由硬件自动合入视频帧。这保证了 RTSP 流中自带检测框。

## 6. 模块同步机制

| **监控对象**      | **实现机制**               | **关键函数/参数**                                      |
| ----------------- | -------------------------- | ------------------------------------------------------ |
| **VENC 编码完成** | **阻塞等待 (Blocking)**    | `RK_MPI_VENC_GetStream` 的 `s32MilliSec` 参数设为 `-1` |
| **NPU 推理完成**  | **同步调用 (Synchronous)** | `inference_retinaface_model` 直至计算结束才返回        |
| **VI 数据就绪**   | **循环轮询/阻塞**          | 在推理线程中主动请求帧数据 `RK_MPI_VI_GetChnFrame`     |

这些阻塞其实可以接受，毕竟本来就是在对应线程中的操作，而不是同一个线程中的同步操作。这个线程阻塞了就是阻塞了，也没有优化空间。

## 7. 关于毕业设计软件部分

在此基础上实现：**WebRTC推流**，显示在一个经过设计的网页里，在该网页里，可以动态调整模型类型（例如从`yolov5`切换为`retainface`）。

### 7.1 WebRTC

复用 `venc` 的码流即可，引入第三方**WebRTC**库，准备选择`libdatachannel`。

### 7.2 AI模型动态切换

注意到luckfox提供的例程中，对于不同的AI模型，虽然推理、处理和绘制的逻辑有很大不同，但是都有几个实现这些功能的线程，也就是说，可以直接用**线程隔离**，而不是去做复杂的接口设计和动态切换。

这里牵扯到一个问题，就是**RKNN模型**的加载问题，如果只是简单的线程切换的话，所有RKNN模型在启动阶段就要加载进内存中，对于RV1106来说，这些内存占用不能忽视。

为此需要实现一个热加载逻辑，不仅仅要维护不同AI模型线程的切换，还有做到资源的管理，实现“**先卸载、再加载**”。
