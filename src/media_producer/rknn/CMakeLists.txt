# ========================================
# rknn 模块 CMakeLists.txt
# RKNN 工具库 - 图像处理和类型定义
# ========================================

# RKNN 工具文件
set(RKNN_SOURCES
    image_utils.cpp
)

set(RKNN_HEADERS
    ai_types.h
    image_utils.h
)

# ========================================
# RKNN 库路径配置
# ========================================

# RKNN 头文件目录（包含 rknn_api.h, yolov5.h, postprocess.h 等）
set(RKNN_INCLUDE_DIR ${LUCKFOX_MPI_INCLUDE_DIR}/rknn CACHE PATH "RKNN include directory")

# RGA 头文件目录
set(RGA_INCLUDE_DIR ${LUCKFOX_MPI_INCLUDE_DIR}/librga CACHE PATH "RGA include directory")

# OpenCV-mobile 配置
set(OPENCV_MOBILE_DIR ${CMAKE_SOURCE_DIR}/thirdparty/luckfox_pico_rkmpi_example)
set(OPENCV_INCLUDE_DIR ${OPENCV_MOBILE_DIR}/include)
set(OPENCV_LIB_DIR ${OPENCV_MOBILE_DIR}/lib/uclibc/lib)

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
        ${CMAKE_CURRENT_SOURCE_DIR}/..       # 访问 media_producer/ 下的其他模块
        ${LUCKFOX_MPI_INCLUDE_DIR}           # 包含 rk_mpi_*.h
        ${RKNN_INCLUDE_DIR}                  # 包含 rknn_api.h 等
        ${RGA_INCLUDE_DIR}                   # 包含 im2d.hpp 等
        ${OPENCV_INCLUDE_DIR}                # OpenCV-mobile 头文件

        # for rknn_box_priors.h
        ${CMAKE_SOURCE_DIR}/thirdparty/luckfox_pico_rkmpi_example/luckfox_pico_rtsp_retinaface_osd/include
)

# 链接库目录
target_link_directories(rknn_lib
    PUBLIC
        ${LUCKFOX_MPI_LIB_DIR}
        ${OPENCV_LIB_DIR}
)

# 链接依赖库
target_link_libraries(rknn_lib
    PUBLIC
        ${RKNN_LIBS}
        rga
        opencv_imgproc
        opencv_core
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
