# ========================================
# yolov5 模块 CMakeLists.txt
# YOLOv5 目标检测模式 - Producer + Model
# ========================================

set(YOLOV5_SOURCES
    yolo_producer.cpp
    yolov5_model.cpp
)

set(YOLOV5_HEADERS
    yolo_producer.h
    yolov5_model.h
    mpi_config.h
)

# ========================================
# 创建静态库
# ========================================

add_library(yolov5_lib STATIC ${YOLOV5_SOURCES} ${YOLOV5_HEADERS})

# 设置头文件包含目录
target_include_directories(yolov5_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src/media_producer   # for i_media_producer.h
        ${CMAKE_SOURCE_DIR}/src/media_producer/rknn  # for ai_model_base.h, ai_types.h
        ${CMAKE_SOURCE_DIR}/src/media_producer/rkvideo  # for osd_overlay.h
        ${LUCKFOX_MPI_INCLUDE_DIR}
        ${LUCKFOX_MPI_INCLUDE_DIR}/rknn
        ${LUCKFOX_MPI_INCLUDE_DIR}/librga
)

# 链接库目录
target_link_directories(yolov5_lib
    PUBLIC
        ${LUCKFOX_MPI_LIB_DIR}
)

# 链接依赖库
target_link_libraries(yolov5_lib
    PUBLIC
        rknn_lib       # RKNN 运行时核心
        rkvideo        # 视频处理
    PRIVATE
        common
        spdlog::spdlog
)

# 设置编译选项
target_compile_options(yolov5_lib
    PRIVATE
        -Wall
        -Wextra
)

# RV1106 特定宏定义
target_compile_definitions(yolov5_lib
    PRIVATE
        RV1106_1103
)
