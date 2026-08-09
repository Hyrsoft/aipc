if(AIPC_ENABLE_RV1106)
    find_package(Threads REQUIRED)
    if(NOT TARGET Rockchip::MediaRuntime)
        message(FATAL_ERROR "Rockchip::MediaRuntime is required for video_decode_worker")
    endif()
    if(NOT TARGET aipc_rockchip_drm)
        message(FATAL_ERROR "libdrm is required for the RV1106 VDEC runtime")
    endif()
    add_executable(video_decode_worker src/main.cpp)
    target_link_libraries(video_decode_worker PRIVATE
        Aipc::NativeCommon
        Rockchip::MediaRuntime
        aipc_rockchip_rockit_full
        aipc_rockchip_drm
        Threads::Threads
        m
    )
    set_target_properties(video_decode_worker PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib;/oem/usr/lib;/oem/lib")
    install(TARGETS video_decode_worker RUNTIME DESTINATION bin)
endif()
