# ========================================
# rtsp 静态库
# ========================================

set(RTSP_SOURCES
    rk_rtsp.cpp
)

set(RTSP_HEADERS
    rk_rtsp.h
)

add_library(rtsp_lib STATIC ${RTSP_SOURCES} ${RTSP_HEADERS})

target_include_directories(rtsp_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${LUCKFOX_MPI_INCLUDE_DIR}
)

# 链接 Luckfox MPI 库目录
target_link_directories(rtsp_lib
    PUBLIC
        ${LUCKFOX_MPI_LIB_DIR}
)

target_link_libraries(rtsp_lib
    PUBLIC
        common
        rtsp  # librtsp.a 包含 rtsp_demo 函数
    PRIVATE
        spdlog::spdlog
)
