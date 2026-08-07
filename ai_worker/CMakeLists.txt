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

if(AIPC_ENABLE_RV1106)
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

if(AIPC_NATIVE_BUILD_TESTS)
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
