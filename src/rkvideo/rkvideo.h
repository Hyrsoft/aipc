/**
 * @file rkvideo.h
 * @brief RK 视频模块 - 基于 Luckfox RKMPI 的视频采集与编码
 *
 * 提供：
 * - VI/VENC 模块的初始化与反初始化
 * - 编码流的分发（到 RTSP/WebRTC）
 * - 原始帧的获取（供 AI 推理使用）
 *
 * @author 好软，好温暖
 * @date 2026-01-30
 */

#pragma once

#include "common/media_buffer.h"

// ============================================================================
// 视频配置参数
// ============================================================================

struct VideoConfig {
    int width = 1920;
    int height = 1080;
    int frameRate = 30;
    int bitRate = 10 * 1024;  // kbps
};

// ============================================================================
// 核心接口
// ============================================================================

/**
 * @brief 初始化 rkvideo 模块
 * 
 * 初始化 ISP、VI、VENC 等子系统，建立视频采集与编码链路
 * 
 * @return 0 成功，-1 失败
 */
int rkvideo_init();

/**
 * @brief 反初始化 rkvideo 模块
 * 
 * 释放所有资源，停止视频采集与编码
 * 
 * @return 0 成功
 */
int rkvideo_deinit();

// ============================================================================
// 流分发接口 - 用于 RTSP/WebRTC
// ============================================================================

/**
 * @brief 流消费者回调类型
 */
using VideoStreamCallback = void(*)(EncodedStreamPtr stream, void* userData);

/**
 * @brief 注册编码流消费者
 * 
 * @param name 消费者名称
 * @param callback 回调函数
 * @param userData 用户数据
 * @param queueSize 队列大小（背压控制）
 */
void rkvideo_register_stream_consumer(const char* name, VideoStreamCallback callback, 
                                       void* userData, int queueSize = 3);

/**
 * @brief 启动流分发
 */
void rkvideo_start_streaming();

/**
 * @brief 停止流分发
 */
void rkvideo_stop_streaming();

// ============================================================================
// 原始帧接口 - 用于 AI 推理
// ============================================================================

/**
 * @brief 从 VI 通道 1 获取原始帧
 * 
 * @param timeoutMs 超时时间（毫秒），-1 表示阻塞等待
 * @return VideoFramePtr 成功返回帧指针，失败返回 nullptr
 * 
 * @note 返回的智能指针会自动管理 MPI 资源释放
 */
VideoFramePtr rkvideo_get_vi_frame(int timeoutMs = -1);

/**
 * @brief 获取当前视频配置
 */
const VideoConfig& rkvideo_get_config();