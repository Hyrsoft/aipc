find_package(Threads REQUIRED)

add_library(media_worker_config STATIC
    src/config.cpp
    src/event_emitter.cpp
)
target_include_directories(media_worker_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(media_worker_config PUBLIC
    Aipc::NativeOptions
    nlohmann_json::nlohmann_json
)

add_library(media_worker_video_ipc STATIC src/video_ipc_publisher.cpp)
add_library(media_worker_audio_ipc STATIC src/audio_ipc_publisher.cpp)
add_library(media_worker_ai_ipc STATIC src/ai_frame_ipc_publisher.cpp)
add_library(media_worker_control STATIC src/media_control.cpp)
add_library(media_worker_metrics STATIC src/metrics_sampler.cpp)

foreach(target
        media_worker_video_ipc
        media_worker_audio_ipc
        media_worker_ai_ipc
        media_worker_control
        media_worker_metrics)
    target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(${target} PUBLIC Aipc::NativeOptions Threads::Threads)
endforeach()
target_link_libraries(media_worker_ai_ipc PUBLIC Aipc::NativeCommon)
target_link_libraries(media_worker_control PUBLIC
    Aipc::NativeCommon
    nlohmann_json::nlohmann_json)

if(AIPC_ENABLE_RV1106)
    if(NOT TARGET Rockchip::MediaRuntime)
        message(FATAL_ERROR "Rockchip::MediaRuntime is required for media_worker")
    endif()
    add_executable(media_worker
        src/main.cpp
        src/media_worker.cpp
        src/media_runtime.cpp
        src/media_control_service.cpp
        src/ai_input_channel.cpp
        src/video_pipeline.cpp
        src/rgn_manager.cpp
        src/audio_pipeline.cpp
    )
    target_include_directories(media_worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(media_worker PRIVATE
        Aipc::NativeOptions
        media_worker_config
        media_worker_metrics
        media_worker_video_ipc
        media_worker_audio_ipc
        media_worker_ai_ipc
        media_worker_control
        Rockchip::MediaRuntime
        Threads::Threads
        dl
        m
    )
    set_target_properties(media_worker PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib;/oem/usr/lib;/oem/lib")
    install(TARGETS media_worker RUNTIME DESTINATION bin)
    install(FILES config/media_worker.example.json DESTINATION config)
endif()

if(AIPC_NATIVE_BUILD_TESTS)
    enable_testing()
    function(aipc_media_test name source library)
        add_executable(${name} ${source})
        target_link_libraries(${name} PRIVATE ${library} Aipc::NativeOptions)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()
    aipc_media_test(media_worker_config_tests tests/config_test.cpp media_worker_config)
    aipc_media_test(media_worker_video_ipc_tests tests/video_ipc_publisher_test.cpp media_worker_video_ipc)
    aipc_media_test(media_worker_audio_ipc_tests tests/audio_ipc_publisher_test.cpp media_worker_audio_ipc)
    aipc_media_test(media_worker_metrics_tests tests/metrics_sampler_test.cpp media_worker_metrics)
    aipc_media_test(media_worker_ai_ipc_tests tests/ai_frame_ipc_publisher_test.cpp media_worker_ai_ipc)
endif()
