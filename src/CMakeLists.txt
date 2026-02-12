# ========================================
# src 目录 CMakeLists.txt
# 创建 aipc 可执行文件
# ========================================

# 添加公共模块（必须在其他模块之前）
add_subdirectory(common)

# 添加子模块（静态库）
add_subdirectory(media_producer)     # 视频采集和 AI 推理模块
add_subdirectory(media_distribution) # 流分发模块（RTSP/WebRTC/WS/File）
add_subdirectory(httpserver)

# ========================================
# aipc 可执行文件
# ========================================

set(AIPC_SOURCES
    main.cpp
    http.cpp
)

# 创建可执行文件
add_executable(aipc ${AIPC_SOURCES})

# 设置头文件包含目录
target_include_directories(aipc
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/media_producer
        ${CMAKE_CURRENT_SOURCE_DIR}/media_distribution
        ${LIBDATACHANNEL_PATH}/deps/json/single_include  # nlohmann/json
)

# 链接静态库和第三方库
# 注意：链接顺序很重要，被依赖的库放后面
target_link_libraries(aipc
    PRIVATE
        common
        httpserver_lib      # HTTP 服务器模块
        media_distribution  # 流分发模块
        media_producer      # 视频采集和 AI 推理模块
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
