cmake_minimum_required(VERSION 3.15)

project(media_worker VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MEDIA_WORKER_BUILD_RUNTIME "Build the RV1106 media worker executable" ON)
option(MEDIA_WORKER_BUILD_TESTS "Build host-side configuration tests" OFF)

get_filename_component(MEDIA_WORKER_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(MEDIA_WORKER_DEFAULT_SDK_ROOT "${MEDIA_WORKER_REPO_ROOT}/.." ABSOLUTE)

set(MEDIA_WORKER_SDK_ROOT "${MEDIA_WORKER_DEFAULT_SDK_ROOT}" CACHE PATH
    "Luckfox Pico SDK root")
set(MEDIA_WORKER_MPI_ROOT
    "${MEDIA_WORKER_REPO_ROOT}/3rdparty/luckfox_pico_rkmpi_example" CACHE PATH
    "Luckfox RKMPI headers and libraries")
set(MEDIA_WORKER_JSON_INCLUDE_DIR
    "${MEDIA_WORKER_REPO_ROOT}/3rdparty/libdatachannel/deps/json/include" CACHE PATH
    "nlohmann/json include directory")

add_library(media_worker_config STATIC
    src/config.cpp
    src/event_emitter.cpp
)
add_library(media_worker_video_ipc STATIC src/video_ipc_publisher.cpp)
add_library(media_worker_metrics STATIC src/metrics_sampler.cpp)
target_include_directories(media_worker_metrics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(media_worker_metrics PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(media_worker_video_ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(media_worker_video_ipc PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(media_worker_video_ipc PUBLIC pthread)
target_include_directories(media_worker_config
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MEDIA_WORKER_JSON_INCLUDE_DIR}
)
target_compile_options(media_worker_config PRIVATE -Wall -Wextra -Wpedantic)

if(MEDIA_WORKER_BUILD_RUNTIME)
    set(MEDIA_WORKER_MPI_INCLUDE_DIR "${MEDIA_WORKER_MPI_ROOT}/include")
    set(MEDIA_WORKER_MPI_LIB_DIR "${MEDIA_WORKER_MPI_ROOT}/lib/uclibc")

    foreach(required_path
            "${MEDIA_WORKER_MPI_INCLUDE_DIR}/rk_mpi_sys.h"
            "${MEDIA_WORKER_MPI_LIB_DIR}/librockit.so"
            "${MEDIA_WORKER_MPI_LIB_DIR}/libsample_comm.a")
        if(NOT EXISTS "${required_path}")
            message(FATAL_ERROR "Required RV1106 dependency not found: ${required_path}")
        endif()
    endforeach()

    add_executable(media_worker
        src/main.cpp
        src/media_worker.cpp
        src/video_pipeline.cpp
        src/audio_pipeline.cpp
    )
    target_include_directories(media_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MEDIA_WORKER_MPI_INCLUDE_DIR}
        ${MEDIA_WORKER_MPI_INCLUDE_DIR}/rkaiq
        ${MEDIA_WORKER_MPI_INCLUDE_DIR}/rkaiq/uAPI2
        ${MEDIA_WORKER_MPI_INCLUDE_DIR}/rkaiq/common
        ${MEDIA_WORKER_MPI_INCLUDE_DIR}/rkaiq/xcore
        ${MEDIA_WORKER_MPI_INCLUDE_DIR}/rkaiq/algos
        ${MEDIA_WORKER_MPI_INCLUDE_DIR}/rkaiq/iq_parser
        ${MEDIA_WORKER_MPI_INCLUDE_DIR}/rkaiq/iq_parser_v2
    )
    target_link_directories(media_worker PRIVATE ${MEDIA_WORKER_MPI_LIB_DIR})
    target_compile_definitions(media_worker PRIVATE
        ISP_HW_V30
        RKPLATFORM=ON
        ARCH64=OFF
        UAPI2
        _LARGEFILE_SOURCE
        _LARGEFILE64_SOURCE
        _FILE_OFFSET_BITS=64
    )
    target_compile_options(media_worker PRIVATE
        -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections
    )
    target_link_options(media_worker PRIVATE
        -Wl,--gc-sections
        -Wl,-rpath-link,${MEDIA_WORKER_MPI_LIB_DIR}
    )
    target_link_libraries(media_worker PRIVATE
        media_worker_config
        media_worker_metrics
        media_worker_video_ipc
        sample_comm
        rockit
        rockchip_mpp
        rkaiq
        rga
        pthread
        dl
        m
    )
    set_target_properties(media_worker PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib;/oem/usr/lib;/oem/lib"
        BUILD_WITH_INSTALL_RPATH OFF
    )

    if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
        set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH
            "Install prefix" FORCE)
    endif()
    install(TARGETS media_worker RUNTIME DESTINATION bin)
    install(FILES config/media_worker.example.json DESTINATION config)
    install(PROGRAMS scripts/run_on_board.sh DESTINATION bin)
    install(PROGRAMS scripts/board_restart_test.sh DESTINATION bin)
    install(PROGRAMS scripts/board_negative_test.sh DESTINATION bin)
endif()

if(MEDIA_WORKER_BUILD_TESTS)
    enable_testing()
    add_executable(media_worker_config_tests tests/config_test.cpp)
    target_link_libraries(media_worker_config_tests PRIVATE media_worker_config)
    target_compile_options(media_worker_config_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME media_worker_config_tests COMMAND media_worker_config_tests)
    add_executable(media_worker_video_ipc_tests tests/video_ipc_publisher_test.cpp)
    target_link_libraries(media_worker_video_ipc_tests PRIVATE media_worker_video_ipc)
    target_compile_options(media_worker_video_ipc_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME media_worker_video_ipc_tests COMMAND media_worker_video_ipc_tests)
    add_executable(media_worker_metrics_tests tests/metrics_sampler_test.cpp)
    target_link_libraries(media_worker_metrics_tests PRIVATE media_worker_metrics)
    target_compile_options(media_worker_metrics_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME media_worker_metrics_tests COMMAND media_worker_metrics_tests)
endif()
