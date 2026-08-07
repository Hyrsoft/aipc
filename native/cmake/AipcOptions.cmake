function(aipc_create_build_options)
    if(TARGET aipc_native_options)
        return()
    endif()
    add_library(aipc_native_options INTERFACE)
    add_library(Aipc::NativeOptions ALIAS aipc_native_options)
    target_compile_options(aipc_native_options INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<BOOL:${AIPC_ENABLE_RV1106}>>:-ffunction-sections;-fdata-sections>
    )
    target_link_options(aipc_native_options INTERFACE
        $<$<BOOL:${AIPC_ENABLE_RV1106}>:-Wl,--gc-sections>
    )
    target_compile_definitions(aipc_native_options INTERFACE
        $<$<BOOL:${AIPC_ENABLE_RV1106}>:ISP_HW_V30;RKPLATFORM=ON;ARCH64=OFF;UAPI2;_LARGEFILE_SOURCE;_LARGEFILE64_SOURCE;_FILE_OFFSET_BITS=64>
    )
endfunction()
