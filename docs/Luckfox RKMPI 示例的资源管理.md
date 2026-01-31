分析 [`luckfox_pico_rtsp_retinaface_osd` 例程](https://github.com/LuckfoxTECH/luckfox_pico_rkmpi_example/blob/kernel-5.10.160/example/luckfox_pico_rtsp_retinaface_osd/src/main.cc)


## 一、venc编码和rtsp推流线程

对于venc编码来说，vi采集到venc编码，视频帧是自动流转，零拷贝的

一旦ISP产生一帧原始图像，硬件自动将其投递给编码器，不需要我们用代码处理，所以对于这个线程，主要关注获取venc编码后的h.264包，并发送给rtsp进行推流

```cpp
static void *GetMediaBuffer(void *arg) {
	(void)arg;
	printf("========%s========\n", __func__);
	void *pData = RK_NULL;
	int s32Ret;

	VENC_STREAM_S stFrame;
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));

	while (1) {
    
    // 获取编码后的H.264包
		s32Ret = RK_MPI_VENC_GetStream(0, &stFrame, -1);
		if (s32Ret == RK_SUCCESS) {
			if (g_rtsplive && g_rtsp_session) {
				pData = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
				rtsp_tx_video(g_rtsp_session, (uint8_t *)pData, stFrame.pstPack->u32Len,
				              stFrame.pstPack->u64PTS);
				rtsp_do_event(g_rtsplive);
			}

      // 资源释放，释放编码后的包
			s32Ret = RK_MPI_VENC_ReleaseStream(0, &stFrame);
			if (s32Ret != RK_SUCCESS) {
				RK_LOGE("RK_MPI_VENC_ReleaseStream fail %x", s32Ret);
			}
		}
    
    // 暂停10ms
		usleep(10 * 1000);
	}
	printf("\n======exit %s=======\n", __func__);
  
  // 线程结束时释放stFrame
	free(stFrame.pstPack);
	return NULL;
}
```

其中，主要的帧格式是`VENC_STREAM_S`，可以看到它封装了venc支持的编码格式

```cpp
/* Defines the features of an stream */
typedef struct rkVENC_STREAM_S {
    VENC_PACK_S ATTRIBUTE* pstPack;            /* R; stream pack attribute*/
    RK_U32      ATTRIBUTE u32PackCount;        /* R; the pack number of one frame stream*/
    RK_U32      u32Seq;                        /* R; the list number of stream*/

    union {
        VENC_STREAM_INFO_H264_S   stH264Info;                        /* R; the stream info of h264*/
        VENC_STREAM_INFO_JPEG_S   stJpegInfo;                        /* R; the stream info of jpeg*/
        VENC_STREAM_INFO_H265_S   stH265Info;                        /* R; the stream info of h265*/
        VENC_STREAM_INFO_PRORES_S stProresInfo;                      /* R; the stream info of prores*/
    };

    union {
        VENC_STREAM_ADVANCE_INFO_H264_S   stAdvanceH264Info;         /* R; the stream info of h264*/
        VENC_STREAM_ADVANCE_INFO_JPEG_S   stAdvanceJpegInfo;         /* R; the stream info of jpeg*/
        VENC_STREAM_ADVANCE_INFO_H265_S   stAdvanceH265Info;         /* R; the stream info of h265*/
        VENC_STREAM_ADVANCE_INFO_PRORES_S stAdvanceProresInfo;       /* R; the stream info of prores*/
    };
} VENC_STREAM_S;
```

## 二、NPU推理线程

利用`RK_MPI_VI_GetChnFrame`从VI通道获取原始数据帧，再利用opencv-mobile进行格式转换和缩放，最后用RGN，在VENC编码前将OSD合入视频帧

格式转换：将 **NV12** 转换为 **BGR**

缩放：将图像适配模型输入的 **640×640** 尺寸

```cpp
static void *RetinaProcessBuffer(void *arg) {
	(void)arg;
	printf("========%s========\n", __func__);

	int disp_width  = DISP_WIDTH;
	int disp_height = DISP_HEIGHT;
	int model_width = 640;
	int model_height = 640;
	
	char text[16];	
	float scale_x = (float)disp_width / (float)model_width;  
	float scale_y = (float)disp_height / (float)model_height;   
	int sX,sY,eX,eY;

	int s32Ret;
	int group_count = 0;
  
  // 这是rkmpi中最核心最原始的数据帧，用它来承接从VI通道中获取的原始图像
	VIDEO_FRAME_INFO_S stViFrame;

	while(1)
	{
    // 手动抓取，主动向VI的通道1（专门用于推理的）请求一帧数据
		s32Ret = RK_MPI_VI_GetChnFrame(0, 1, &stViFrame, -1);
    
    // 之后利用opencv-mobile，对数据帧进行格式转换和缩放
		if(s32Ret == RK_SUCCESS)
		{
      // 获取虚拟地址
			void *vi_data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);
			if(vi_data != RK_NULL)
			{
        // 使用cv::Mat对原始数据帧进行封装，并进行格式转换和缩放处理
				cv::Mat yuv420sp(disp_height + disp_height / 2, disp_width, CV_8UC1, vi_data);
				cv::Mat bgr(disp_height, disp_width, CV_8UC3);			
				cv::Mat model_bgr(model_height, model_width, CV_8UC3);			

				cv::cvtColor(yuv420sp, bgr, cv::COLOR_YUV420sp2BGR);

				cv::resize(bgr, model_bgr, cv::Size(model_width ,model_height), 0, 0, cv::INTER_LINEAR);	
        
        // memcpy到RKNN输入内存
				memcpy(rknn_app_ctx.input_mems[0]->virt_addr, model_bgr.data, model_width * model_height * 3);
        
        // 阻塞，该函数计算结束才会返回
        // od_results为推理结果
				inference_retinaface_model(&rknn_app_ctx, &od_results);

        // 利用rgn，将osd覆盖到venc的输入数据上，实现标注
				for(int i = 0; i < od_results.count; i++)
				{					
					object_detect_result *det_result = &(od_results.results[i]);
					
					if(i == 0)
					{
						sX = (int)((float)det_result->box.left 	 *scale_x);	
						sY = (int)((float)det_result->box.top 	 *scale_y);	
						eX = (int)((float)det_result->box.right  *scale_x);	
						eY = (int)((float)det_result->box.bottom *scale_y);
						printf("%d %d %d %d\n",sX,sY,eX,eY);

						sX = sX - (sX % 2);
						sY = sY - (sY % 2);
						eX = eX	- (eX % 2);				
						eY = eY	- (eY % 2);					
					
						if((eX > sX) && (eY > sY) && (sX > 0) && (sY > 0))
						{
							test_rgn_overlay_line_process(sX,sY,0,i);
							test_rgn_overlay_line_process(eX,sY,1,i);
							test_rgn_overlay_line_process(eX,eY,2,i);
							test_rgn_overlay_line_process(sX,eY,3,i);
						}
						group_count++;
					}

				}		

			}
      
      // 释放，如果不调用的话，
			s32Ret = RK_MPI_VI_ReleaseChnFrame(0, 1, &stViFrame);
			if (s32Ret != RK_SUCCESS) {
				RK_LOGE("RK_MPI_VI_ReleaseChnFrame fail %x", s32Ret);
			}
		}
		else{
			printf("Get viframe error %d !\n", s32Ret);
			continue;
		}

    // 暂停500ms
		usleep(500000);		
		for(int i = 0;i < group_count; i++)
			rgn_overlay_release(i);
		group_count = 0;
	}			
	return NULL;
}
```
该线程操作的数据帧：`VIDEO_FRAME_INFO_S`

```cpp
typedef struct rkVIDEO_FRAME_S {
    MB_BLK              pMbBlk; // 底层内存块句柄，该结构体本质上是对它的一层封装
    RK_U32              u32Width;
    RK_U32              u32Height;
    RK_U32              u32VirWidth;
    RK_U32              u32VirHeight;
    VIDEO_FIELD_E       enField;
    PIXEL_FORMAT_E      enPixelFormat;
    VIDEO_FORMAT_E      enVideoFormat;
    COMPRESS_MODE_E     enCompressMode;
    DYNAMIC_RANGE_E     enDynamicRange;
    COLOR_GAMUT_E       enColorGamut;

    RK_VOID            *pVirAddr[RK_MAX_COLOR_COMPONENT]; //虚拟地址

    RK_U32              u32TimeRef;
    RK_U64              u64PTS;

    RK_U64              u64PrivateData;
    RK_U32              u32FrameFlag;     /* FRAME_FLAG_E, can be OR operation. */
} VIDEO_FRAME_S;

typedef struct rkVIDEO_FRAME_INFO_S {
    VIDEO_FRAME_S stVFrame;
} VIDEO_FRAME_INFO_S;
```

## 三、毕设思路

也就是说，对于我这个需求，我需要处理两种格式，一是venc编码后的h.264视频流，二是从VI通道直接获取的原始数据帧。rkmpi实际上已经做了一定程度的封装了，我应该在它封装的基础上进行管理，而不是拆开自己封装。对于h.264视频流，主要涉及rtsp和webrtc的使用，会有一个竞争或者共享的关系，而对于推理用的原始数据帧，它的路径和所有权比较明确，毕竟同时只会有一个AI推理的线程存在
