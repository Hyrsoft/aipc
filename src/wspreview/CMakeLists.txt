# ========================================
# wspreview 模块 CMakeLists.txt
# WebSocket H.264 裸流预览
# ========================================

set(WSPREVIEW_SOURCES
    ws_preview.cpp
)

add_library(wspreview_lib STATIC ${WSPREVIEW_SOURCES})

target_include_directories(wspreview_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
    PRIVATE
        ${LIBDATACHANNEL_PATH}/include
        ${LUCKFOX_MPI_INCLUDE_DIR}
)

target_link_libraries(wspreview_lib
    PUBLIC
        common
    PRIVATE
        LibDataChannel::LibDataChannel
)
