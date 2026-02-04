# ========================================
# common 公共模块
# ========================================

# 创建 INTERFACE 库（header-only）
add_library(common INTERFACE)

# 包含目录指向 src，这样可以使用 #include "common/logger.h"
target_include_directories(common
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(common
    INTERFACE
        spdlog::spdlog
        asio
)
