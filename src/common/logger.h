/**
 * @file logger.h
 * @brief 日志管理器 - 基于 spdlog 的模块化日志系统
 *
 * 提供模块级别的日志管理，支持：
 * - 编译时日志级别控制（通过 SPDLOG_ACTIVE_LEVEL）
 * - 模块独立的 logger 实例
 * - 统一的日志格式
 * 
 * 使用方式：
 * 1. 在模块的 .cpp 文件顶部定义 LOG_TAG
 *    #define LOG_TAG "ModuleName"
 * 2. 使用 LOG_INFO, LOG_ERROR 等宏进行日志输出
 *
 * @author 好软，好温暖
 * @date 2026-01-30
 */

#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>
#include <string>
#include <mutex>

/**
 * @class LogManager
 * @brief 日志管理器单例类
 * 
 * 管理所有模块的 logger 实例，提供统一的日志配置
 */
class LogManager {
public:
    /**
     * @brief 获取指定模块的 logger 实例
     * @param name 模块名称
     * @return logger 智能指针
     * 
     * 如果 logger 不存在则创建新的实例并注册
     */
    static std::shared_ptr<spdlog::logger> GetLogger(const std::string& name) {
        auto logger = spdlog::get(name);
        if (!logger) {
            // 创建带颜色的控制台输出 logger
            logger = spdlog::stdout_color_mt(name);
            
            // 设置日志格式：[时间] [线程ID] [模块名] [等级] [源文件:行号] 内容
            // %Y-%m-%d %H:%M:%S.%e : 时间戳（精确到毫秒）
            // %t : 线程ID
            // %n : logger 名称（模块名）
            // %^%l%$ : 带颜色的日志等级
            // %s:%# : 源文件名:行号
            // %v : 日志内容
            logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%n] [%^%l%$] [%s:%#] %v");
            
            // 设置该 logger 的运行时等级为 trace，实际输出受 SPDLOG_ACTIVE_LEVEL 限制
            logger->set_level(spdlog::level::trace);
        }
        return logger;
    }

    /**
     * @brief 全局日志系统初始化
     * 
     * 在 main 函数开始时调用，用于配置全局日志设置
     * 对于嵌入式环境，可以考虑开启异步日志以减少对实时性的影响
     */
    static void Init() {
        // 设置全局日志等级（受限于 SPDLOG_ACTIVE_LEVEL）
        spdlog::set_level(spdlog::level::trace);
        
        // 设置全局日志格式
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%n] [%^%l%$] [%s:%#] %v");
        
        // 异步日志配置（可选，用于高性能场景）
        // spdlog::init_thread_pool(8192, 1);
        
        // 创建默认 logger
        GetLogger("main");
    }

    /**
     * @brief 关闭日志系统
     * 
     * 在程序退出前调用，确保所有日志都被刷新
     */
    static void Shutdown() {
        spdlog::shutdown();
    }
};

// ========================================
// 模块级日志宏定义
// ========================================
// 这些宏使用 SPDLOG_LOGGER_XXX 以支持编译时日志级别控制
// 在编译时，低于 SPDLOG_ACTIVE_LEVEL 的日志会被完全移除（零开销）

// 默认 LOG_TAG，模块应在包含此头文件前定义自己的 LOG_TAG
#ifndef LOG_TAG
#define LOG_TAG "default"
#endif

// 获取当前模块的 logger
#define GET_LOGGER() LogManager::GetLogger(LOG_TAG)

// 日志宏 - 支持编译时级别控制
#define LOG_TRACE(...)    SPDLOG_LOGGER_TRACE(GET_LOGGER(), __VA_ARGS__)
#define LOG_DEBUG(...)    SPDLOG_LOGGER_DEBUG(GET_LOGGER(), __VA_ARGS__)
#define LOG_INFO(...)     SPDLOG_LOGGER_INFO(GET_LOGGER(), __VA_ARGS__)
#define LOG_WARN(...)     SPDLOG_LOGGER_WARN(GET_LOGGER(), __VA_ARGS__)
#define LOG_ERROR(...)    SPDLOG_LOGGER_ERROR(GET_LOGGER(), __VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_LOGGER_CRITICAL(GET_LOGGER(), __VA_ARGS__)
