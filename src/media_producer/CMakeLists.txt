# ========================================
# media_producer 模块 CMakeLists.txt
# 媒体生产者 - 策略模式实现的视频采集与编码
# ========================================

# ========================================
# 源文件配置
# ========================================

# 接口和管理器（核心层）
set(MEDIA_PRODUCER_CORE_SOURCES
    media_manager.cpp
)

set(MEDIA_PRODUCER_CORE_HEADERS
    i_media_producer.h
    media_manager.h
)

# 合并核心源文件
set(MEDIA_PRODUCER_SOURCES
    ${MEDIA_PRODUCER_CORE_SOURCES}
)

set(MEDIA_PRODUCER_HEADERS
    ${MEDIA_PRODUCER_CORE_HEADERS}
)

# MPI / NPU / ISP 依赖库（原先由已删除的旧 AI 模块间接提供）
set(MEDIA_PRODUCER_MPI_LIBS
    sample_comm
    rockit
    rockchip_mpp
    rkaiq
    rga
    rknnmrt
    rockiva
    ivs
    rve
    pthread
    dl
)

# ========================================
# 添加子模块
# ========================================
add_subdirectory(simple_ipc)         # SimpleIPC 纯监控模式
add_subdirectory(visiong)            # VisionG 库驱动的 AI 推理模式

# ========================================
# 创建静态库
# ========================================

add_library(media_producer STATIC 
    ${MEDIA_PRODUCER_SOURCES} 
    ${MEDIA_PRODUCER_HEADERS}
)

# 设置头文件包含目录
target_include_directories(media_producer
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..   # 访问 common/
    PRIVATE
        ${LUCKFOX_MPI_INCLUDE_DIR}
)

    target_link_directories(media_producer
        PUBLIC
        ${LUCKFOX_MPI_LIB_DIR}
        ${VISIONG_EXTRA_LIB_DIR}
    )

# 链接依赖库
target_link_libraries(media_producer
    PUBLIC
        common
        simple_ipc_lib   # SimpleIPC 纯监控模式
        visiong_producer_lib  # VisionG 库驱动的 AI 推理
    ${MEDIA_PRODUCER_MPI_LIBS}
)

# 设置编译选项
target_compile_options(media_producer
    PRIVATE
        -Wall
        -Wextra
)
