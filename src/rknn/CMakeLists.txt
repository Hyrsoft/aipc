# ========================================
# rknn 模块 CMakeLists.txt
# AI 推理引擎 - 基于 RKNN 的模型管理与调度
# ========================================

set(RKNN_SOURCES
    ai_engine.cpp
    yolov5_model.cpp
    retinaface_model.cpp
)

set(RKNN_HEADERS
    ai_types.h
    ai_model_base.h
    ai_engine.h
    yolov5_model.h
    retinaface_model.h
)

# ========================================
# RKNN 库路径配置
# ========================================

# RKNN 头文件目录（包含 rknn_api.h, yolov5.h, postprocess.h 等）
set(RKNN_INCLUDE_DIR ${LUCKFOX_MPI_INCLUDE_DIR}/rknn CACHE PATH "RKNN include directory")

# RKNN 运行时库（librknnmrt）
set(RKNN_LIBS
    rknnmrt
    CACHE INTERNAL "RKNN runtime libraries"
)

# ========================================
# 创建静态库
# ========================================

add_library(rknn_lib STATIC ${RKNN_SOURCES} ${RKNN_HEADERS})

# 设置头文件包含目录
target_include_directories(rknn_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${LUCKFOX_MPI_INCLUDE_DIR}          # 包含 rk_mpi_*.h
        ${RKNN_INCLUDE_DIR}                  # 包含 rknn_api.h 等
)

# 链接库目录
target_link_directories(rknn_lib
    PUBLIC
        ${LUCKFOX_MPI_LIB_DIR}
)

# 链接依赖库
target_link_libraries(rknn_lib
    PUBLIC
        ${RKNN_LIBS}
    PRIVATE
        common
        spdlog::spdlog
)

# 设置编译选项
target_compile_options(rknn_lib
    PRIVATE
        -Wall
        -Wextra
)

# RV1106 特定宏定义（启用零拷贝 DMA Buffer 支持）
target_compile_definitions(rknn_lib
    PRIVATE
        RV1106_1103
)
