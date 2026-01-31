# ========================================
# src 目录 CMakeLists.txt
# 创建 aipc 可执行文件
# ========================================

# 添加公共模块（必须在其他模块之前）
add_subdirectory(common)

# 添加子模块（静态库）
add_subdirectory(rkvideo)
add_subdirectory(rtsp)
add_subdirectory(webrtc)
add_subdirectory(file)
add_subdirectory(httpserver)

# ========================================
# aipc 可执行文件
# ========================================

set(AIPC_SOURCES
    main.cpp
    thread_stream.cpp
    http.cpp
)

# 创建可执行文件
add_executable(aipc ${AIPC_SOURCES})

# 设置头文件包含目录
target_include_directories(aipc
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${LIBDATACHANNEL_PATH}/deps/json/single_include  # nlohmann/json
)

# 链接静态库和第三方库
# 注意：链接顺序很重要，被依赖的库放后面
target_link_libraries(aipc
    PRIVATE
        common
        httpserver_lib  # HTTP 服务器模块
        rtsp_lib        # rtsp_lib 依赖 sample_comm
        file_lib        # file_lib 依赖 FFmpeg 和 rkvideo
        webrtc_lib      # WebRTC 模块
        rkvideo         # rkvideo 也依赖 sample_comm，放后面确保符号解析
        spdlog::spdlog
        httplib
)

# 设置编译选项
target_compile_options(aipc
    PRIVATE
        -Wall
        -Wextra
)

# 设置输出目录
set_target_properties(aipc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
