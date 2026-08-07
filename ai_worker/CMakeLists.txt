cmake_minimum_required(VERSION 3.15)

project(ai_worker VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AI_WORKER_ENABLE_VISIONG "Build the VisionG inference backend" OFF)
set(AI_WORKER_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../target/ai-deps" CACHE PATH
    "Directory populated by scripts/fetch-ai-deps.sh")
set(AI_WORKER_MPI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/luckfox_pico_rkmpi_example"
    CACHE PATH "Luckfox RKMPI headers")
set(AI_WORKER_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
    "Luckfox Pico SDK root")
set(AI_WORKER_JSON_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/nlohmann_json/include" CACHE PATH
    "nlohmann/json include directory")

set(LUA_SRC_DIR "${AI_WORKER_DEPS_DIR}/lua/src")
set(LUA_SOURCES
    lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c ldebug.c
    ldo.c ldump.c lfunc.c lgc.c linit.c liolib.c llex.c lmathlib.c lmem.c
    loadlib.c lobject.c lopcodes.c loslib.c lparser.c lstate.c lstring.c
    lstrlib.c ltable.c ltablib.c ltm.c lundump.c lutf8lib.c lvm.c lzio.c
)
list(TRANSFORM LUA_SOURCES PREPEND "${LUA_SRC_DIR}/")
foreach(source ${LUA_SOURCES})
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "Lua source not found: ${source}; run scripts/fetch-ai-deps.sh")
    endif()
endforeach()

add_library(aipc_lua STATIC ${LUA_SOURCES})
target_include_directories(aipc_lua PUBLIC "${LUA_SRC_DIR}")
target_compile_definitions(aipc_lua PRIVATE LUA_USE_POSIX)
target_link_libraries(aipc_lua PUBLIC m dl)

add_executable(ai_worker src/main.cpp)
target_include_directories(ai_worker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${AI_WORKER_JSON_INCLUDE_DIR}
    ${AI_WORKER_MPI_ROOT}/include
)
target_link_libraries(ai_worker PRIVATE aipc_lua pthread dl m)
target_compile_options(ai_worker PRIVATE -Wall -Wextra -Wpedantic)

if(AI_WORKER_ENABLE_VISIONG)
    set(VisionG_DIR "${AI_WORKER_DEPS_DIR}/visiong/cmake" CACHE PATH
        "VisionG package configuration" FORCE)
    find_package(VisionG 1.2.1 REQUIRED CONFIG NO_DEFAULT_PATH)
    target_link_libraries(ai_worker PRIVATE VisionG::visiong)
    target_link_directories(ai_worker PRIVATE
        "${AI_WORKER_MPI_ROOT}/lib/uclibc"
        "${AI_WORKER_SDK_ROOT}/media/out/lib"
    )
    target_link_options(ai_worker PRIVATE
        "-Wl,-rpath-link,${AI_WORKER_MPI_ROOT}/lib/uclibc"
        "-Wl,-rpath-link,${AI_WORKER_SDK_ROOT}/media/out/lib"
    )
    target_compile_definitions(ai_worker PRIVATE AIPC_ENABLE_VISIONG=1)
else()
    target_compile_definitions(ai_worker PRIVATE AIPC_ENABLE_VISIONG=0)
endif()

set_target_properties(ai_worker PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib;/oem/usr/lib;/oem/lib"
    BUILD_WITH_INSTALL_RPATH OFF
)
install(TARGETS ai_worker RUNTIME DESTINATION bin)

enable_testing()
add_test(
    NAME ai_worker_lua_validation
    COMMAND ai_worker
        --project-dir "${CMAKE_CURRENT_SOURCE_DIR}/examples/yolov5"
        --models-dir "${CMAKE_CURRENT_SOURCE_DIR}/examples/yolov5"
        --validate-only
        --mock
)
