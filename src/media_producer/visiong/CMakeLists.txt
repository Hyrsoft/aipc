# ========================================
# visiong 模块 CMakeLists.txt
# VisionG 库驱动的 AI 推理模式 - Producer + 模型策略
# ========================================

set(VISIONG_PRODUCER_SOURCES
    visiong_producer.cpp
    yolov5/yolov5_strategy.cpp
    python/python_strategy.cpp
)

set(VISIONG_PRODUCER_HEADERS
    visiong_producer.h
    i_model_strategy.h
    yolov5/yolov5_strategy.h
    python/python_strategy.h
)

# ========================================
# 创建静态库
# ========================================

add_library(visiong_producer_lib STATIC ${VISIONG_PRODUCER_SOURCES} ${VISIONG_PRODUCER_HEADERS})

# 设置头文件包含目录
target_include_directories(visiong_producer_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src/media_producer   # for i_media_producer.h
        ${VISIONG_INCLUDE_DIR}
        ${LUCKFOX_MPI_INCLUDE_DIR}
        ${LUCKFOX_MPI_INCLUDE_DIR}/librga
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/uAPI2
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/common
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/xcore
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/iq_parser
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/iq_parser_v2
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/algos
)

# 链接依赖库
target_link_libraries(visiong_producer_lib
    PUBLIC
        visiong_runtime  # VisionG 预编译库
    PRIVATE
        common
        spdlog::spdlog
        ${PYTHON_LIBRARY}
)

# pybind11 头文件（用于 PythonStrategy 的 embed 模式）
target_include_directories(visiong_producer_lib
    PRIVATE
        ${PYBIND11_INCLUDE_DIR}
        ${PYTHON_INCLUDE_DIR}
)

# 设置编译选项
target_compile_options(visiong_producer_lib
    PRIVATE
        -Wall
        -Wextra
)

# RV1106 特定宏定义
target_compile_definitions(visiong_producer_lib
    PRIVATE
        RV1106_1103
)
