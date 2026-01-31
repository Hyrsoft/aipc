# ========================================
# file 静态库 - 文件保存模块
# ========================================

# Buildroot sysroot（包含 FFmpeg）
set(BR_SYSROOT "$ENV{HOME}/projects/luckfox-pico/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot")

set(FILE_SOURCES
    file_saver.cpp
    thread_file.cpp
)

set(FILE_HEADERS
    file_saver.h
    thread_file.h
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
        ${BR_SYSROOT}/usr/include  # FFmpeg 头文件
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
    z           # zlib
    m           # math
)

# 链接目录
target_link_directories(file_lib
    PUBLIC
        ${LUCKFOX_MPI_LIB_DIR}
        ${BR_SYSROOT}/usr/lib  # FFmpeg 及其依赖库
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

