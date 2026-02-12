# ========================================
# media_producer 模块 CMakeLists.txt
# 媒体生产者 - 策略模式实现的视频采集与编码
# ========================================

# ========================================
# 源文件配置
# ========================================

# 接口和管理器
set(MEDIA_PRODUCER_CORE_SOURCES
    media_manager.cpp
)

set(MEDIA_PRODUCER_CORE_HEADERS
    i_media_producer.h
    media_manager.h
)

# SimpleIPC 模式
set(SIMPLE_IPC_SOURCES
    simple_ipc/simple_ipc_producer.cpp
)

set(SIMPLE_IPC_HEADERS
    simple_ipc/simple_ipc_producer.h
    simple_ipc/mpi_config.h
)

# YOLOv5 模式
set(YOLO_SOURCES
    yolov5/yolo_producer.cpp
)

set(YOLO_HEADERS
    yolov5/yolo_producer.h
)

# RetinaFace 模式
set(RETINAFACE_SOURCES
    retainface/retinaface_producer.cpp
)

set(RETINAFACE_HEADERS
    retainface/retinaface_producer.h
)

# 合并所有源文件
set(MEDIA_PRODUCER_SOURCES
    ${MEDIA_PRODUCER_CORE_SOURCES}
    ${SIMPLE_IPC_SOURCES}
    ${YOLO_SOURCES}
    ${RETINAFACE_SOURCES}
)

set(MEDIA_PRODUCER_HEADERS
    ${MEDIA_PRODUCER_CORE_HEADERS}
    ${SIMPLE_IPC_HEADERS}
    ${YOLO_HEADERS}
    ${RETINAFACE_HEADERS}
)

# ========================================
# 添加子模块
# ========================================
add_subdirectory(rkvideo)            # 视频采集管道
add_subdirectory(rknn)               # RKNN 运行时核心
add_subdirectory(yolov5)             # YOLOv5 目标检测模式
add_subdirectory(retainface)         # RetinaFace 人脸检测模式

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

# 链接依赖库
target_link_libraries(media_producer
    PUBLIC
        common
        rkvideo          # 视频管道封装
        rknn_lib         # RKNN 运行时核心
        yolov5_lib       # YOLOv5 目标检测
        retinaface_lib   # RetinaFace 人脸检测
)

# 设置编译选项
target_compile_options(media_producer
    PRIVATE
        -Wall
        -Wextra
)
