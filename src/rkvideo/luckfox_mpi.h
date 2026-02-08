#ifndef __LUCKFOX_MPI_H
#define __LUCKFOX_MPI_H

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "sample_comm.h"
#include "rk_mpi_vpss.h"

#define TEST_ARGB32_PIX_SIZE 4
#define TEST_ARGB32_RED 0xFF0000FF
#define TEST_ARGB32_GREEN 0x00FF00FF
#define TEST_ARGB32_BLUE 0x0000FFFF
#define TEST_ARGB32_TRANS 0x00000000
#define TEST_ARGB32_BLACK 0x000000FF


RK_U64 TEST_COMM_GetNowUs();
RK_S32 test_rgn_overlay_line_process(int sX ,int sY,int type, int group);
RK_S32 rgn_overlay_release(int group);

int vi_dev_init();
int vi_chn_init(int channelId, int width, int height);

/**
 * @brief 初始化 VPSS Group 和通道
 * 
 * 创建 VPSS Group 0，配置两个输出通道：
 * - Chn0: 全分辨率输出 -> 绑定到 VENC（编码流）
 * - Chn1: 可配置分辨率输出 -> 用户 GetFrame（AI 推理）
 * 
 * @param grpId VPSS Group ID
 * @param inputWidth 输入宽度（与 VI 输出一致）
 * @param inputHeight 输入高度（与 VI 输出一致）
 * @param chn0Width Chn0 输出宽度（给 VENC）
 * @param chn0Height Chn0 输出高度（给 VENC）
 * @param chn1Width Chn1 输出宽度（给 AI），可选，<=0 则不启用 Chn1
 * @param chn1Height Chn1 输出高度（给 AI）
 * @return 0 成功，-1 失败
 */
int vpss_init(int grpId, int inputWidth, int inputHeight,
              int chn0Width, int chn0Height,
              int chn1Width = 0, int chn1Height = 0);

/**
 * @brief 销毁 VPSS Group 和通道
 * 
 * @param grpId VPSS Group ID
 * @param enableChn1 是否启用了 Chn1
 * @return 0 成功
 */
int vpss_deinit(int grpId, bool enableChn1 = false);

/**
 * @brief 动态重配置 VPSS Chn1 的输出分辨率
 * 
 * 在 AI 模型切换导致输入尺寸变化时调用，修改 Chn1 的输出分辨率
 * 以匹配新模型的需求。
 * 
 * @param grpId VPSS Group ID
 * @param width 新的输出宽度
 * @param height 新的输出高度
 * @return 0 成功，-1 失败
 * 
 * @note 需要先 Disable 通道再重新配置并 Enable
 */
int vpss_reconfigure_chn1(int grpId, int width, int height);

int venc_init(int chnId, int width, int height, RK_CODEC_ID_E enType);

#endif