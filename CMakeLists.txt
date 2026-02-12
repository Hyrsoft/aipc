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
set(LUCKFOX_MPI_INCLUDE_DIR ${LUCKFOX_MPI_PATH}/include CACHE PATH "Luckfox MPI include path")
set(LUCKFOX_MPI_LIB_DIR ${LUCKFOX_MPI_PATH}/lib/uclibc CACHE PATH "Luckfox MPI library path")
set(SPDLOG_PATH ${CMAKE_SOURCE_DIR}/thirdparty/spdlog CACHE PATH "spdlog library path")
set(CPP_HTTPLIB_PATH ${CMAKE_SOURCE_DIR}/thirdparty/cpp-httplib CACHE PATH "cpp-httplib library path")
set(LIBDATACHANNEL_PATH ${CMAKE_SOURCE_DIR}/thirdparty/libdatachannel CACHE PATH "libdatachannel library path")
set(ASIO_PATH ${CMAKE_SOURCE_DIR}/thirdparty/asio CACHE PATH "Asio library path")

# ========================================
# OpenSSL 配置（用于交叉编译）
# ========================================
# Buildroot 编译输出中包含 OpenSSL
set(BUILDROOT_SYSROOT "/home/hao/projects/luckfox-pico/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot")
set(OPENSSL_ROOT_DIR "${BUILDROOT_SYSROOT}/usr" CACHE PATH "OpenSSL root directory" FORCE)
set(OPENSSL_INCLUDE_DIR "${BUILDROOT_SYSROOT}/usr/include" CACHE PATH "OpenSSL include directory" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${BUILDROOT_SYSROOT}/usr/lib/libcrypto.so" CACHE FILEPATH "OpenSSL crypto library" FORCE)
set(OPENSSL_SSL_LIBRARY "${BUILDROOT_SYSROOT}/usr/lib/libssl.so" CACHE FILEPATH "OpenSSL SSL library" FORCE)
# 创建 OpenSSL 导入目标，避免 find_package 搜索失败
set(OPENSSL_FOUND TRUE CACHE BOOL "OpenSSL found" FORCE)
set(OPENSSL_LIBRARIES "${OPENSSL_SSL_LIBRARY};${OPENSSL_CRYPTO_LIBRARY}" CACHE STRING "OpenSSL libraries" FORCE)

# ========================================
# 添加第三方库
# ========================================

# spdlog (静态库模式 - 强制静态链接，避免嵌入式设备上的动态库问题)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "Build spdlog as static library" FORCE)
add_subdirectory(${SPDLOG_PATH} ${CMAKE_BINARY_DIR}/spdlog)

# cpp-httplib (header-only)
add_library(httplib INTERFACE)
target_include_directories(httplib INTERFACE ${CPP_HTTPLIB_PATH})

# Asio (header-only, standalone mode)
add_library(asio INTERFACE)
target_include_directories(asio INTERFACE ${ASIO_PATH}/asio/include)
target_compile_definitions(asio INTERFACE 
    ASIO_STANDALONE
    ASIO_NO_DEPRECATED
)
# Asio 需要 pthread
target_link_libraries(asio INTERFACE pthread)

# libdatachannel (WebRTC 支持)
# 配置选项
set(NO_WEBSOCKET OFF CACHE BOOL "Disable WebSocket support" FORCE)
set(NO_MEDIA OFF CACHE BOOL "Disable media support" FORCE)  # 需要启用媒体支持以使用 H264RtpPacketizer
set(NO_EXAMPLES ON CACHE BOOL "Disable examples" FORCE)
set(NO_TESTS ON CACHE BOOL "Disable tests" FORCE)
set(USE_NICE OFF CACHE BOOL "Use libnice for ICE" FORCE)
add_subdirectory(${LIBDATACHANNEL_PATH} ${CMAKE_BINARY_DIR}/libdatachannel)

# ========================================
# 添加源代码子目录
# ========================================
add_subdirectory(src)

# ========================================
# 安装配置
# ========================================
# 设置安装前缀（默认安装到 build/install）
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Install path prefix" FORCE)
endif()

# 安装目录设置
set(AIPC_INSTALL_BIN_DIR "bin")
set(AIPC_INSTALL_LIB_DIR "lib")
set(AIPC_INSTALL_ASSETS_DIR "assets")
set(AIPC_INSTALL_WWW_DIR "www")
set(AIPC_INSTALL_MODEL_DIR "model")

# 安装可执行文件
install(TARGETS aipc
    RUNTIME DESTINATION ${AIPC_INSTALL_BIN_DIR}
)

# 注意：libdatachannel 动态库已手动部署到设备中，无需安装

# 安装启动/停止脚本
install(PROGRAMS 
    ${CMAKE_SOURCE_DIR}/assets/start_app.sh
    ${CMAKE_SOURCE_DIR}/assets/stop_app.sh
    DESTINATION ${AIPC_INSTALL_BIN_DIR}
)

# 安装 AI 模型文件
install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/model/
    DESTINATION ${AIPC_INSTALL_MODEL_DIR}
    FILES_MATCHING 
    PATTERN "*.rknn"
    PATTERN "*.txt"
)

# 安装 Web 资源文件 (from www/dist)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/www/dist/
    DESTINATION ${AIPC_INSTALL_WWW_DIR}
    FILES_MATCHING
    PATTERN "*.html"
    PATTERN "*.js"
    PATTERN "*.css"
    PATTERN "*.ico"
    PATTERN "*.png"
    PATTERN "*.jpg"
    PATTERN "*.svg"
)

# 安装其他资源文件（图片等）
install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/images/
    DESTINATION ${AIPC_INSTALL_ASSETS_DIR}/images
    FILES_MATCHING
    PATTERN "*.png"
    PATTERN "*.jpg"
    PATTERN "*.bmp"
)

# 生成版本信息文件
file(WRITE ${CMAKE_BINARY_DIR}/version.txt 
    "AIPC Version: ${PROJECT_VERSION}\n"
    "Build Type: ${CMAKE_BUILD_TYPE}\n"
    "Build Date: ${CMAKE_CURRENT_SOURCE_DIR}\n"
)
install(FILES ${CMAKE_BINARY_DIR}/version.txt DESTINATION ${AIPC_INSTALL_BIN_DIR})

# ========================================
# 输出配置信息
# ========================================
message(STATUS "========================================")
message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "spdlog compile-time log level: ${SPDLOG_ACTIVE_LEVEL} (0=TRACE, 2=INFO, 6=OFF)")
message(STATUS "========================================")
