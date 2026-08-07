cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ai_worker VERSION 0.1.0 LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    option(AI_WORKER_ENABLE_VISIONG "Build VisionG backend" OFF)
    option(AI_WORKER_BUILD_TESTS "Build ai_worker tests" ON)
    set(AIPC_ENABLE_RV1106 ${AI_WORKER_ENABLE_VISIONG})
    set(AIPC_NATIVE_BUILD_TESTS ${AI_WORKER_BUILD_TESTS})
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../native/cmake")
    include(AipcDependencies)
    include(AipcOptions)
    aipc_setup_dependencies()
    aipc_create_build_options()
endif()

option(AI_WORKER_ENABLE_VISIONG "Build VisionG backend" OFF)
option(AI_WORKER_BUILD_TESTS "Build ai_worker tests" ON)
find_package(Threads REQUIRED)

add_executable(ai_worker
    src/main.cpp
    src/application.cpp
    src/backend.cpp
    src/lua_runtime.cpp
    src/manifest.cpp
)
target_include_directories(ai_worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ai_worker PRIVATE
    Aipc::NativeOptions
    Aipc::NativeCommon
    Aipc::Lua
    nlohmann_json::nlohmann_json
    Threads::Threads
    dl
    m
)

if(AI_WORKER_ENABLE_VISIONG)
    if(NOT TARGET VisionG::visiong OR NOT TARGET Rockchip::VisionGRuntime)
        message(FATAL_ERROR "VisionG and Rockchip runtime targets are required")
    endif()
    target_link_libraries(ai_worker PRIVATE VisionG::visiong Rockchip::VisionGRuntime)
    target_compile_definitions(ai_worker PRIVATE AIPC_ENABLE_VISIONG=1)
else()
    target_compile_definitions(ai_worker PRIVATE AIPC_ENABLE_VISIONG=0)
endif()

set_target_properties(ai_worker PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib;/oem/usr/lib;/oem/lib")
install(TARGETS ai_worker RUNTIME DESTINATION bin)
install(FILES THIRD_PARTY.md DESTINATION licenses RENAME AI_WORKER_THIRD_PARTY.md)
install(FILES
    examples/yolov5/manifest.json
    examples/yolov5/main.lua
    DESTINATION seed/ai/projects/yolov5-coco80)

if(AI_WORKER_BUILD_TESTS)
    enable_testing()
    add_test(
        NAME ai_worker_lua_validation
        COMMAND ai_worker
            --project-dir "${CMAKE_CURRENT_SOURCE_DIR}/examples/yolov5"
            --models-dir "${CMAKE_CURRENT_SOURCE_DIR}/examples/yolov5"
            --validate-only
            --mock
    )
endif()
