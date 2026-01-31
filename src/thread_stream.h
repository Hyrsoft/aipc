/**
 * @file thread_stream.h
 * @brief 流处理线程接口
 *
 * 提供 RTSP/WebRTC/文件保存等流输出的统一管理接口
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#pragma once

#include <string>
#include "rtsp/rk_rtsp.h"

// ============================================================================
// 流输出配置
// ============================================================================

/**
 * @brief 流输出总配置
 */
struct StreamOutputConfig {
    bool enableRtsp = true;           ///< 是否启用 RTSP 推流
    bool enableWebrtc = false;        ///< 是否启用 WebRTC 推流
    bool enableFile = false;          ///< 是否启用文件保存
    
    RtspConfig rtspConfig;            ///< RTSP 配置
    std::string filePath;             ///< 录制文件路径
};

// ============================================================================
// RTSP 推流接口
// ============================================================================

/**
 * @brief 初始化 RTSP 推流
 * 
 * 初始化 RTSP 服务器并注册到流分发器
 * 
 * @param config RTSP 配置
 * @return true 成功，false 失败
 */
bool stream_rtsp_init(const RtspConfig& config = RtspConfig{});

/**
 * @brief 反初始化 RTSP 推流
 */
void stream_rtsp_deinit();

// ============================================================================
// WebRTC 推流接口（待实现）
// ============================================================================

/**
 * @brief 初始化 WebRTC 推流
 * @return true 成功，false 失败
 */
bool stream_webrtc_init();

/**
 * @brief 反初始化 WebRTC 推流
 */
void stream_webrtc_deinit();

// ============================================================================
// 文件保存接口（待实现）
// ============================================================================

/**
 * @brief 开始录制到文件
 * 
 * @param filename 文件路径
 * @return true 成功，false 失败
 */
bool stream_file_start(const char* filename);

/**
 * @brief 停止录制
 */
void stream_file_stop();

// ============================================================================
// 统一流管理接口
// ============================================================================

/**
 * @brief 初始化流管理器
 * 
 * 根据配置初始化所有启用的流输出
 * 
 * @param config 流输出配置
 * @return true 成功，false 失败
 */
bool stream_manager_init(const StreamOutputConfig& config = StreamOutputConfig{});

/**
 * @brief 反初始化流管理器
 */
void stream_manager_deinit();

/**
 * @brief 启动所有流输出
 */
void stream_manager_start();

/**
 * @brief 停止所有流输出
 */
void stream_manager_stop();
