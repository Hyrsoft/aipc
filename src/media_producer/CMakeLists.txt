# ========================================
# media_producer 模块 CMakeLists.txt
# 媒体生产者 - 策略模式实现的视频采集与编码
# ========================================

# 查找 OpenCV (全局提供给各个 Producer)
set(OpenCV_DIR ${CMAKE_SOURCE_DIR}/thirdparty/visiong/3rdparty/opencv/lib/cmake/opencv4)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)

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

# ========================================
# 添加子模块
# ========================================
add_subdirectory(common)             # 共用工具（AI类型、图像处理、OSD）
add_subdirectory(simple_ipc)         # SimpleIPC 纯监控模式
add_subdirectory(yolov5)             # YOLOv5 目标检测模式
add_subdirectory(retainface)         # RetinaFace 人脸检测模式
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

# 链接依赖库
target_link_libraries(media_producer
    PUBLIC
        common
        media_common     # AI类型、图像处理、OSD
        simple_ipc_lib   # SimpleIPC 纯监控模式
        yolov5_lib       # YOLOv5 目标检测
        retinaface_lib   # RetinaFace 人脸检测
        visiong_producer_lib  # VisionG 库驱动的 AI 推理
)

# 设置编译选项
target_compile_options(media_producer
    PRIVATE
        -Wall
        -Wextra
)
