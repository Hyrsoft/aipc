# ========================================
# retainface 模块 CMakeLists.txt
# RetinaFace 人脸检测模式 - Producer + Model
# ========================================

set(RETINAFACE_SOURCES
    retinaface_producer.cpp
    retinaface_model.cpp
)

set(RETINAFACE_HEADERS
    retinaface_producer.h
    retinaface_model.h
    mpi_config.h
)

# ========================================
# 创建静态库
# ========================================

add_library(retinaface_lib STATIC ${RETINAFACE_SOURCES} ${RETINAFACE_HEADERS})

# 设置头文件包含目录
target_include_directories(retinaface_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src/media_producer   # for i_media_producer.h
        ${CMAKE_SOURCE_DIR}/src/media_producer/rknn  # for ai_model_base.h, ai_types.h
        ${CMAKE_SOURCE_DIR}/src/media_producer/rkvideo  # for osd_overlay.h
        ${LUCKFOX_MPI_INCLUDE_DIR}
        ${LUCKFOX_MPI_INCLUDE_DIR}/rknn
        ${LUCKFOX_MPI_INCLUDE_DIR}/librga
        
        # for rknn_box_priors.h (RetinaFace prior boxes)
        ${CMAKE_SOURCE_DIR}/thirdparty/luckfox_pico_rkmpi_example/luckfox_pico_rtsp_retinaface_osd/include
)

# 链接库目录
target_link_directories(retinaface_lib
    PUBLIC
        ${LUCKFOX_MPI_LIB_DIR}
)

# 链接依赖库
target_link_libraries(retinaface_lib
    PUBLIC
        rknn_lib       # RKNN 运行时核心
        rkvideo        # 视频处理
    PRIVATE
        common
        spdlog::spdlog
)

# 设置编译选项
target_compile_options(retinaface_lib
    PRIVATE
        -Wall
        -Wextra
)

# RV1106 特定宏定义
target_compile_definitions(retinaface_lib
    PRIVATE
        RV1106_1103
)
