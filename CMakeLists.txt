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
set(LUCKFOX_MPI_PATH ${CMAKE_SOURCE_DIR}/3rdparty/luckfox_pico_rkmpi_example CACHE PATH "Luckfox MPI library path")
set(LUCKFOX_MPI_INCLUDE_DIR ${LUCKFOX_MPI_PATH}/include CACHE PATH "Luckfox MPI include path")
set(LUCKFOX_MPI_LIB_DIR ${LUCKFOX_MPI_PATH}/lib/uclibc CACHE PATH "Luckfox MPI library path")
set(SPDLOG_PATH ${CMAKE_SOURCE_DIR}/3rdparty/spdlog CACHE PATH "spdlog library path")
set(CPP_HTTPLIB_PATH ${CMAKE_SOURCE_DIR}/3rdparty/cpp-httplib CACHE PATH "cpp-httplib library path")
set(LIBDATACHANNEL_PATH ${CMAKE_SOURCE_DIR}/3rdparty/libdatachannel CACHE PATH "libdatachannel library path")
set(ASIO_PATH ${CMAKE_SOURCE_DIR}/3rdparty/asio CACHE PATH "Asio library path")

# VisionG 预编译库路径（将 visiong_cpp.zip 解压到该目录）
set(VISIONG_PREBUILT_DIR ${CMAKE_BINARY_DIR}/visiong_cpp CACHE PATH "VisionG prebuilt package root")
set(VISIONG_INCLUDE_DIR ${VISIONG_PREBUILT_DIR}/include CACHE PATH "VisionG include dir")
set(VISIONG_LIB_DIR ${VISIONG_PREBUILT_DIR}/lib CACHE PATH "VisionG library dir")
set(VISIONG_EXTRA_LIB_DIR ${CMAKE_SOURCE_DIR}/../media/out/lib CACHE PATH "Extra vendor libs for VisionG (ivs/rve)")

# VisionG Python 模块路径（由用户手工部署，不自动下载）
set(VISIONG_PYTHON_PREBUILT_DIR "" CACHE PATH "VisionG Python package root (manual deployment)")

# ========================================
# Python 3.11 配置（用于 pybind11::embed）
# ========================================
# 使用 visiong 源码中的 target_python（交叉编译目标板 Python）
set(VISIONG_TARGET_PYTHON_DIR ${CMAKE_SOURCE_DIR}/3rdparty/visiong/3rdparty/target_python)
set(PYTHON_INCLUDE_DIR ${VISIONG_TARGET_PYTHON_DIR}/include/python3.11 CACHE PATH "Python include dir")
set(PYTHON_LIBRARY ${VISIONG_TARGET_PYTHON_DIR}/lib/libpython3.11.so CACHE FILEPATH "Python library")
set(PYBIND11_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/3rdparty/visiong/3rdparty/pybind11 CACHE PATH "pybind11 include dir")

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

# spdlog (动态库模式)
set(SPDLOG_BUILD_SHARED ON CACHE BOOL "Build spdlog as shared library" FORCE)
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

# VisionG (预编译库导入 - 自动下载)
set(VISIONG_ZIP_URL "https://github.com/yiex/visiong/releases/download/v1.0.1/visiong_cpp.zip")
set(VISIONG_ZIP_PATH "${CMAKE_BINARY_DIR}/visiong_cpp.zip")

if(NOT EXISTS "${VISIONG_INCLUDE_DIR}/visiong/core/Camera.h")
    if(NOT EXISTS "${VISIONG_ZIP_PATH}")
        message(STATUS "Downloading VisionG prebuilt library from ${VISIONG_ZIP_URL} ...")
        file(DOWNLOAD "${VISIONG_ZIP_URL}" "${VISIONG_ZIP_PATH}"
            STATUS VISIONG_DL_STATUS
            SHOW_PROGRESS
            TLS_VERIFY ON
        )
        list(GET VISIONG_DL_STATUS 0 VISIONG_DL_CODE)
        if(NOT VISIONG_DL_CODE EQUAL 0)
            list(GET VISIONG_DL_STATUS 1 VISIONG_DL_MSG)
            file(REMOVE "${VISIONG_ZIP_PATH}")
            message(FATAL_ERROR "Failed to download VisionG: ${VISIONG_DL_MSG}")
        endif()
    endif()

    message(STATUS "Extracting VisionG prebuilt library to ${VISIONG_PREBUILT_DIR} ...")
    file(ARCHIVE_EXTRACT INPUT "${VISIONG_ZIP_PATH}" DESTINATION "${VISIONG_PREBUILT_DIR}")

    if(NOT EXISTS "${VISIONG_INCLUDE_DIR}/visiong/core/Camera.h")
        message(FATAL_ERROR "VisionG extraction failed: headers not found at ${VISIONG_INCLUDE_DIR}")
    endif()
endif()

set(VISIONG_LIBRARY "")
if(EXISTS "${VISIONG_LIB_DIR}/libvisiong.so")
    set(VISIONG_LIBRARY "${VISIONG_LIB_DIR}/libvisiong.so")
elseif(EXISTS "${VISIONG_LIB_DIR}/libvisiong.a")
    set(VISIONG_LIBRARY "${VISIONG_LIB_DIR}/libvisiong.a")
else()
    message(FATAL_ERROR "VisionG library not found in ${VISIONG_LIB_DIR} (expected libvisiong.so or libvisiong.a)")
endif()

add_library(visiong_runtime UNKNOWN IMPORTED GLOBAL)
set_target_properties(visiong_runtime PROPERTIES
    IMPORTED_LOCATION "${VISIONG_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${VISIONG_INCLUDE_DIR}"
)

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

# 安装可执行文件，设置 RPATH 让可执行文件能找到 ../lib 下的动态库
set_target_properties(aipc PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib"
    BUILD_WITH_INSTALL_RPATH OFF
)
install(TARGETS aipc
    RUNTIME DESTINATION ${AIPC_INSTALL_BIN_DIR}
)

# 安装 libdatachannel 动态库到项目 lib 目录
if(TARGET datachannel)
    install(TARGETS datachannel
        LIBRARY DESTINATION ${AIPC_INSTALL_LIB_DIR}
        ARCHIVE DESTINATION ${AIPC_INSTALL_LIB_DIR}
    )
endif()

# 安装 spdlog 动态库到项目 lib 目录
if(TARGET spdlog)
    install(TARGETS spdlog
        LIBRARY DESTINATION ${AIPC_INSTALL_LIB_DIR}
        ARCHIVE DESTINATION ${AIPC_INSTALL_LIB_DIR}
    )
endif()

# 安装启动/停止脚本
install(PROGRAMS
    ${CMAKE_SOURCE_DIR}/assets/start_app.sh
    ${CMAKE_SOURCE_DIR}/assets/stop_app.sh
    DESTINATION ${AIPC_INSTALL_BIN_DIR}
)

# 安装 AI 模型文件（仅内置 YOLOv5 模型，其余由用户通过 Web 上传）
install(FILES
    ${CMAKE_SOURCE_DIR}/assets/model/yolov5.rknn
    ${CMAKE_SOURCE_DIR}/assets/model/coco_80_labels_list.txt
    DESTINATION ${AIPC_INSTALL_MODEL_DIR}
)

# 安装 VisionG 动态库（便于部署）
if(EXISTS "${VISIONG_LIB_DIR}/libvisiong.so")
    install(FILES "${VISIONG_LIB_DIR}/libvisiong.so"
        DESTINATION ${AIPC_INSTALL_LIB_DIR}
    )
endif()

# 安装 VisionG 的依赖库 ivs / rve（来自 media/out/lib）
foreach(_vg_dep ivs rve)
    if(EXISTS "${VISIONG_EXTRA_LIB_DIR}/lib${_vg_dep}.so")
        install(FILES "${VISIONG_EXTRA_LIB_DIR}/lib${_vg_dep}.so"
            DESTINATION ${AIPC_INSTALL_LIB_DIR}
        )
    else()
        message(WARNING "lib${_vg_dep}.so not found in ${VISIONG_EXTRA_LIB_DIR}")
    endif()
endforeach()

# 安装 visiong Python 模块（3rdparty/visiong/src/python/visiong.py）
# sys.path 追加了 ../python，即 /root/aipc/python/，将 visiong.py 安装到该目录
set(AIPC_INSTALL_PYTHON_DIR "python")
install(FILES ${CMAKE_SOURCE_DIR}/3rdparty/visiong/src/python/visiong.py
    DESTINATION ${AIPC_INSTALL_PYTHON_DIR}
)

# 安装 Python 模板代码
set(AIPC_INSTALL_TEMPLATES_DIR "python_templates")
install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/python_templates/
    DESTINATION ${AIPC_INSTALL_TEMPLATES_DIR}
    FILES_MATCHING PATTERN "*.py"
)

# 安装默认 Python 工程
set(AIPC_INSTALL_PROJECTS_DIR "python_projects")
install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/python_projects/
    DESTINATION ${AIPC_INSTALL_PROJECTS_DIR}
    FILES_MATCHING PATTERN "*.py"
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
