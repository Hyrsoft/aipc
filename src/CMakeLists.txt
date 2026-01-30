# ========================================
# src 目录 CMakeLists.txt
# 创建 aipc 可执行文件
# ========================================

# 添加子模块（静态库）
add_subdirectory(rkmpi)
add_subdirectory(rtsp)
add_subdirectory(webrtc)

# ========================================
# aipc 可执行文件
# ========================================

set(AIPC_SOURCES
    main.cpp
)

# 创建可执行文件
add_executable(aipc ${AIPC_SOURCES})

# 设置头文件包含目录
target_include_directories(aipc
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# 链接静态库和第三方库
target_link_libraries(aipc
    PRIVATE
        rkmpi
        # rtsp_lib      # 启用后取消注释
        # webrtc_lib    # 启用后取消注释
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
