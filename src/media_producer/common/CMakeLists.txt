# ========================================
# media_producer/common 静态库
# 
# 共用工具模块：AI 类型定义、图像处理、OSD 覆盖
# ========================================

# OpenCV-mobile 配置
set(OpenCV_DIR ${LUCKFOX_MPI_LIB_DIR}/lib/cmake/opencv4)
find_package(OpenCV REQUIRED)

set(COMMON_SOURCES
    image_utils.cpp
    osd_overlay.cpp
)

set(COMMON_HEADERS
    ai_types.h
    image_utils.h
    osd_overlay.h
)

# MPI 库列表（来自 luckfox-pico-rkmpi-example）
set(MPI_LIBS 
    sample_comm
    rockit
    rockchip_mpp
    rkaiq
    rga
    rknnmrt
    pthread
    dl
)

# 创建静态库
add_library(media_common STATIC ${COMMON_SOURCES} ${COMMON_HEADERS})

# 设置头文件包含目录
target_include_directories(media_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..   # 访问 media_producer/ 下其他模块
        ${CMAKE_CURRENT_SOURCE_DIR}/../..  # 访问 src/common/
        ${LUCKFOX_MPI_INCLUDE_DIR}
        ${LUCKFOX_MPI_INCLUDE_DIR}/rknn
        ${LUCKFOX_MPI_INCLUDE_DIR}/librga
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/uAPI2
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/common
        ${OpenCV_INCLUDE_DIRS}
)

# 链接依赖库
target_link_libraries(media_common
    PUBLIC
        ${OpenCV_LIBS}
        ${MPI_LIBS}
    PRIVATE
        common
        spdlog::spdlog
)

# 链接库目录
target_link_directories(media_common
    PUBLIC
        ${LUCKFOX_MPI_LIB_DIR}
)

# 设置编译选项
target_compile_options(media_common
    PRIVATE
        -Wall
        -Wextra
)
