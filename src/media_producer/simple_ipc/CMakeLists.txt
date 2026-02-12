# ========================================
# simple_ipc 模块 CMakeLists.txt
# 纯 IPC 监控模式 - VI -> VPSS -> VENC 硬件绑定
# ========================================

set(SIMPLE_IPC_SOURCES
    simple_ipc_producer.cpp
)

set(SIMPLE_IPC_HEADERS
    simple_ipc_producer.h
    mpi_config.h
)

# ========================================
# 创建静态库
# ========================================

add_library(simple_ipc_lib STATIC ${SIMPLE_IPC_SOURCES} ${SIMPLE_IPC_HEADERS})

# 设置头文件包含目录
target_include_directories(simple_ipc_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src/media_producer   # for i_media_producer.h
        ${LUCKFOX_MPI_INCLUDE_DIR}
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/uAPI2
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/common
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/xcore
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/iq_parser
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/iq_parser_v2
        ${LUCKFOX_MPI_INCLUDE_DIR}/rkaiq/algos
)

# 链接库目录
target_link_directories(simple_ipc_lib
    PUBLIC
        ${LUCKFOX_MPI_LIB_DIR}
)

# 链接依赖库
target_link_libraries(simple_ipc_lib
    PUBLIC
        common      # asio_context, logger, media_buffer
        spdlog::spdlog
)

# 设置编译选项
target_compile_options(simple_ipc_lib
    PRIVATE
        -Wall
        -Wextra
)
