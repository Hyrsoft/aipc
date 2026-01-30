
# ========================================
# rkvideo 静态库
# ========================================

set(RKVIDEO_SOURCES
    luckfox_mpi.cpp
    rkvideo.cpp
)

set(RKVIDEO_HEADERS
    luckfox_mpi.h
    rkvideo.h
    stream_dispatcher.h
)

# ========================================
# MPI 库列表 （取自 luckfox-pico-rkmpi-example）
# ========================================
set(LUCKFOX_MPI_INCLUDE_DIR ${LUCKFOX_MPI_PATH}/include CACHE PATH "Luckfox MPI include directory")
set(LUCKFOX_MPI_LIB_DIR ${LUCKFOX_MPI_PATH}/lib/uclibc CACHE PATH "Luckfox MPI library directory")

set(MPI_LIBS 
    sample_comm
    rockit
    rockchip_mpp
    rkaiq
    rga
    pthread
    dl
)

# 创建静态库
add_library(rkvideo STATIC ${RKVIDEO_SOURCES} ${RKVIDEO_HEADERS})

# 设置头文件包含目录
target_include_directories(rkvideo
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${LUCKFOX_MPI_INCLUDE_DIR}
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/uAPI2
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/common
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/xcore
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/iq_parser
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/iq_parser_v2
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/algos
        
)

# 链接 Luckfox MPI 库目录
target_link_directories(rkvideo
    PUBLIC
        ${LUCKFOX_MPI_LIB_DIR}
)

# 链接依赖库
target_link_libraries(rkvideo
    PUBLIC
        ${MPI_LIBS}
    PRIVATE
        common
        spdlog::spdlog
)

# 设置编译选项
target_compile_options(rkvideo
    PRIVATE
        -Wall
        -Wextra
)