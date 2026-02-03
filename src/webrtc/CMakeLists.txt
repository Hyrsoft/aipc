# ========================================
# webrtc 静态库
# ========================================

set(WEBRTC_SOURCES
    signaling.cpp
    webrtc.cpp
    thread_webrtc.cpp
)

set(WEBRTC_HEADERS
    signaling.h
    webrtc.h
    thread_webrtc.h
)

add_library(webrtc_lib STATIC ${WEBRTC_SOURCES} ${WEBRTC_HEADERS})

# 设置头文件包含目录
target_include_directories(webrtc_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src        # 项目 src 根目录
        ${LIBDATACHANNEL_PATH}/include # libdatachannel 头文件
        ${LIBDATACHANNEL_PATH}/deps/json/single_include  # nlohmann/json
        ${LUCKFOX_MPI_PATH}/include    # RKMPI 头文件
)

# 链接依赖库
target_link_libraries(webrtc_lib
    PUBLIC
        common                         # 公共模块（logger）
        spdlog::spdlog
        datachannel-static             # libdatachannel 静态库
)
