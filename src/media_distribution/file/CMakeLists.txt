# ========================================
# file 静态库 - 文件保存模块
# ========================================

# Buildroot sysroot（包含 FFmpeg）
if(NOT DEFINED BUILDROOT_SYSROOT OR BUILDROOT_SYSROOT STREQUAL "")
    message(FATAL_ERROR "BUILDROOT_SYSROOT is not set, cannot locate FFmpeg headers and libraries")
endif()

set(FILE_SYSROOT "${BUILDROOT_SYSROOT}")
set(FILE_FFMPEG_INCLUDE_DIR "${FILE_SYSROOT}/usr/include")
set(FILE_FFMPEG_LIB_DIR "${FILE_SYSROOT}/usr/lib")

set(FILE_REQUIRED_FFMPEG_HEADERS
    "${FILE_FFMPEG_INCLUDE_DIR}/libavformat/avformat.h"
    "${FILE_FFMPEG_INCLUDE_DIR}/libavcodec/avcodec.h"
    "${FILE_FFMPEG_INCLUDE_DIR}/libavutil/opt.h"
)

foreach(_file_ffmpeg_header IN LISTS FILE_REQUIRED_FFMPEG_HEADERS)
    if(NOT EXISTS "${_file_ffmpeg_header}")
        message(FATAL_ERROR
            "FFmpeg headers are missing from BUILDROOT_SYSROOT='${BUILDROOT_SYSROOT}'. "
            "Expected '${_file_ffmpeg_header}'. "
            "Enable/install FFmpeg in Buildroot or point BUILDROOT_SYSROOT to a sysroot that contains it.")
    endif()
endforeach()

set(FILE_REQUIRED_FFMPEG_LIBS
    avformat
    avcodec
    swresample
    avutil
    swscale
)

foreach(_file_ffmpeg_lib IN LISTS FILE_REQUIRED_FFMPEG_LIBS)
    if(NOT EXISTS "${FILE_FFMPEG_LIB_DIR}/lib${_file_ffmpeg_lib}.so"
       AND NOT EXISTS "${FILE_FFMPEG_LIB_DIR}/lib${_file_ffmpeg_lib}.a")
        message(FATAL_ERROR
            "FFmpeg library 'lib${_file_ffmpeg_lib}' is missing from BUILDROOT_SYSROOT='${BUILDROOT_SYSROOT}'. "
            "Expected it under '${FILE_FFMPEG_LIB_DIR}'.")
    endif()
endforeach()

set(FILE_SOURCES
    file_saver.cpp
    file_service.cpp
)

set(FILE_HEADERS
    file_saver.h
    file_service.h
)

add_library(file_lib STATIC ${FILE_SOURCES} ${FILE_HEADERS})

# 设置头文件包含目录
target_include_directories(file_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${LUCKFOX_MPI_INCLUDE_DIR}
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/uAPI2
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/common
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/xcore
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/iq_parser
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/iq_parser_v2
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/algos
        ${FILE_FFMPEG_INCLUDE_DIR}  # FFmpeg 头文件
)

# ========================================
# FFmpeg 库配置
# ========================================
# FFmpeg 库从 buildroot sysroot 获取

# FFmpeg 库列表
set(FFMPEG_LIBS
    avformat
    avcodec
    swresample
    avutil
    swscale
)

# FFmpeg 依赖的系统库（根据 buildroot 编译配置）
set(FFMPEG_SYS_DEPS
    iconv       # libiconv
    gnutls      # TLS 支持
    tasn1       # gnutls 依赖
    hogweed     # gnutls 依赖
    nettle      # gnutls 依赖
    gmp         # gnutls 依赖
    unistring   # gnutls 依赖
    intl        # gettext
    bz2         # bzip2
    drm         # libdrm
    lzma        # xz/liblzma，libavcodec 依赖
    z           # zlib
    atomic      # ffmpeg 各组件在该 toolchain 下会用到
    m           # math
)

# 链接目录
target_link_directories(file_lib
    PUBLIC
        ${LUCKFOX_MPI_LIB_DIR}
        ${FILE_FFMPEG_LIB_DIR}  # FFmpeg 及其依赖库
)

# 链接依赖库
target_link_libraries(file_lib
    PUBLIC
        common
        ${FFMPEG_LIBS}
        ${FFMPEG_SYS_DEPS}
        rockit    # TDE 支持
        rga       # RGA 加速
    PRIVATE
        spdlog::spdlog
)

# 设置编译选项
target_compile_options(file_lib
    PRIVATE
        -Wall
        -Wextra
)
