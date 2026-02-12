# ========================================
# media_distribution 模块 CMakeLists.txt
# 流分发模块 - RTSP/WebRTC/WebSocket/File
# ========================================

# 添加子模块
add_subdirectory(rtsp)
add_subdirectory(webrtc)
add_subdirectory(wspreview)
add_subdirectory(file)

# ========================================
# 源文件配置
# ========================================

set(MEDIA_DISTRIBUTION_SOURCES
    stream_manager.cpp
)

set(MEDIA_DISTRIBUTION_HEADERS
    stream_manager.h
)

# ========================================
# 创建静态库
# ========================================

add_library(media_distribution STATIC 
    ${MEDIA_DISTRIBUTION_SOURCES} 
    ${MEDIA_DISTRIBUTION_HEADERS}
)

# 设置头文件包含目录
target_include_directories(media_distribution
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# 链接依赖库
target_link_libraries(media_distribution
    PUBLIC
        common
        rtsp_lib
        webrtc_lib
        wspreview_lib
        file_lib
)

# 设置编译选项
target_compile_options(media_distribution
    PRIVATE
        -Wall
        -Wextra
)
