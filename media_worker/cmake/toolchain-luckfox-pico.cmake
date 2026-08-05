set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

get_filename_component(_worker_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(_default_sdk_root "${_worker_root}/../.." ABSOLUTE)

if(DEFINED ENV{LUCKFOX_SDK_ROOT} AND NOT "$ENV{LUCKFOX_SDK_ROOT}" STREQUAL "")
    set(_sdk_root "$ENV{LUCKFOX_SDK_ROOT}")
else()
    set(_sdk_root "${_default_sdk_root}")
endif()
set(MEDIA_WORKER_SDK_ROOT "${_sdk_root}" CACHE PATH "Luckfox Pico SDK root")

set(_toolchain_dir
    "${MEDIA_WORKER_SDK_ROOT}/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf")
set(_tool_prefix "${_toolchain_dir}/bin/arm-rockchip830-linux-uclibcgnueabihf")

if(NOT EXISTS "${_tool_prefix}-g++")
    message(FATAL_ERROR
        "Luckfox cross compiler not found at ${_tool_prefix}-g++. "
        "Set LUCKFOX_SDK_ROOT or MEDIA_WORKER_SDK_ROOT.")
endif()

set(CMAKE_C_COMPILER "${_tool_prefix}-gcc")
set(CMAKE_CXX_COMPILER "${_tool_prefix}-g++")
set(CMAKE_AR "${_tool_prefix}-ar")
set(CMAKE_RANLIB "${_tool_prefix}-ranlib")
set(CMAKE_STRIP "${_tool_prefix}-strip")
set(CMAKE_SYSROOT
    "${_toolchain_dir}/arm-rockchip830-linux-uclibcgnueabihf/sysroot")

set(CMAKE_LINK_DEPENDS_NO_SHARED TRUE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

