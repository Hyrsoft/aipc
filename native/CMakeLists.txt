cmake_minimum_required(VERSION 3.20)

project(aipc_native VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(AIPC_ENABLE_RV1106 "Build RV1106 runtime targets" OFF)
option(AIPC_NATIVE_BUILD_TESTS "Build native host tests" ON)
option(AIPC_FETCHCONTENT_OFFLINE "Disable dependency network access" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(AipcDependencies)
include(AipcOptions)

aipc_setup_dependencies()
aipc_create_build_options()

add_library(aipc_native_common STATIC
    common/src/aipf.cpp
    common/src/io.cpp
    common/src/validation.cpp
)
add_library(Aipc::NativeCommon ALIAS aipc_native_common)
target_include_directories(aipc_native_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/common/include")
target_link_libraries(aipc_native_common PUBLIC
    Aipc::NativeOptions
    nlohmann_json::nlohmann_json)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../media_worker"
                 "${CMAKE_CURRENT_BINARY_DIR}/media_worker")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../ai_worker"
                 "${CMAKE_CURRENT_BINARY_DIR}/ai_worker")

if(AIPC_NATIVE_BUILD_TESTS)
    enable_testing()
    add_executable(aipc_native_protocol_tests common/tests/aipf_test.cpp)
    target_link_libraries(aipc_native_protocol_tests PRIVATE Aipc::NativeCommon)
    target_compile_definitions(aipc_native_protocol_tests PRIVATE
        AIPC_PROTOCOL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../testdata/protocol")
    add_test(NAME aipc_native_protocol_tests COMMAND aipc_native_protocol_tests)
endif()
