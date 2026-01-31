# ========================================
# httpserver 静态库 - HTTP 服务器模块
# ========================================

set(HTTPSERVER_SOURCES
    http_server.cpp
)

set(HTTPSERVER_HEADERS
    http_server.h
)

add_library(httpserver_lib STATIC ${HTTPSERVER_SOURCES} ${HTTPSERVER_HEADERS})

# 设置头文件包含目录
target_include_directories(httpserver_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# 链接依赖库
target_link_libraries(httpserver_lib
    PUBLIC
        common
        httplib       # cpp-httplib (header-only, 但需要链接 pthread)
    PRIVATE
        spdlog::spdlog
)

# cpp-httplib 需要的编译定义
target_compile_definitions(httpserver_lib
    PRIVATE
        # 如果不需要 SSL，取消下面的注释
        # CPPHTTPLIB_NO_OPENSSL
)
