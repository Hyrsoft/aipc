cmake_minimum_required(VERSION 3.15)
project(aipc VERSION 1.0.0 LANGUAGES C CXX)

# ========================================
# 全局编译设置
# ========================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 当编译选项为Debug时，启用调试符号
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g")

# ========================================
# spdlog 编译时日志级别控制
# ========================================
# 设置此选项可在编译时完全移除低于该级别的日志代码（零开销）
# 定义日志等级常量
set(LOG_LEVEL_TRACE    0)
set(LOG_LEVEL_DEBUG    1)
set(LOG_LEVEL_INFO     2)
set(LOG_LEVEL_WARN     3)
set(LOG_LEVEL_ERROR    4)
set(LOG_LEVEL_CRITICAL 5)
set(LOG_LEVEL_OFF      6)

# 根据构建类型自动设置默认日志等级
# Debug 模式下默认 DEBUG，Release 模式下默认 INFO
if(NOT DEFINED SPDLOG_ACTIVE_LEVEL)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(SPDLOG_ACTIVE_LEVEL ${LOG_LEVEL_DEBUG} CACHE STRING "Compile-time log level")
    else()
        set(SPDLOG_ACTIVE_LEVEL ${LOG_LEVEL_INFO} CACHE STRING "Compile-time log level")
    endif()
endif()

add_compile_definitions(SPDLOG_ACTIVE_LEVEL=${SPDLOG_ACTIVE_LEVEL})

# 针对嵌入式环境的优化编译选项
add_compile_options(-ffunction-sections -fdata-sections)
add_link_options(-Wl,--gc-sections)

# ========================================
# 第三方库路径（全局可用）
# ========================================
set(LUCKFOX_MPI_PATH ${CMAKE_SOURCE_DIR}/thirdparty/luckfox_pico_rkmpi_example CACHE PATH "Luckfox MPI library path")
set(SPDLOG_PATH ${CMAKE_SOURCE_DIR}/thirdparty/spdlog CACHE PATH "spdlog library path")
set(CPP_HTTPLIB_PATH ${CMAKE_SOURCE_DIR}/thirdparty/cpp-httplib CACHE PATH "cpp-httplib library path")
set(LIBDATACHANNEL_PATH ${CMAKE_SOURCE_DIR}/thirdparty/libdatachannel CACHE PATH "libdatachannel library path")

# ========================================
# 添加第三方库
# ========================================

# spdlog (header-only 模式)
add_subdirectory(${SPDLOG_PATH} ${CMAKE_BINARY_DIR}/spdlog)

# cpp-httplib (header-only)
add_library(httplib INTERFACE)
target_include_directories(httplib INTERFACE ${CPP_HTTPLIB_PATH})

# libdatachannel (如需要可启用)
# add_subdirectory(${LIBDATACHANNEL_PATH} ${CMAKE_BINARY_DIR}/libdatachannel)

# ========================================
# 添加源代码子目录
# ========================================
add_subdirectory(src)

# ========================================
# 安装配置
# ========================================
# 安装可执行文件
install(TARGETS aipc
    RUNTIME DESTINATION bin
)

# 安装资源文件
install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/
    DESTINATION share/${PROJECT_NAME}
    FILES_MATCHING 
    PATTERN "*.sh"
    PATTERN "*.rknn"
    PATTERN "*.txt"
    PATTERN "*.png"
    PATTERN "*.jpg"
)

# 安装 web 资源
install(DIRECTORY ${CMAKE_SOURCE_DIR}/www/
    DESTINATION share/${PROJECT_NAME}/www
)

# ========================================
# 输出配置信息
# ========================================
message(STATUS "========================================")
message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "spdlog compile-time log level: ${SPDLOG_ACTIVE_LEVEL} (0=TRACE, 2=INFO, 6=OFF)")
message(STATUS "========================================")
