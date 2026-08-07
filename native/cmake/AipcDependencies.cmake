include(FetchContent)

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

set(AIPC_NLOHMANN_JSON_URL
    "https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz")
set(AIPC_NLOHMANN_JSON_SHA256
    "4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187")
set(AIPC_LUA_URL "https://www.lua.org/ftp/lua-5.4.8.tar.gz")
set(AIPC_LUA_SHA256
    "4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae")
set(AIPC_VISIONG_URL
    "https://github.com/yiex/visiong/releases/download/v1.2.1/visiong_cpp.zip")
set(AIPC_VISIONG_SHA256
    "56336cc25150692e21505626b9f359b5dfeaa019f240460c2541b0bfdbe51bc0")
set(AIPC_LUCKFOX_RKMPI_URL
    "https://github.com/LuckfoxTECH/luckfox_pico_rkmpi_example/archive/55178250c05542b156ac94c8c08cecef46589abf.tar.gz")
set(AIPC_LUCKFOX_RKMPI_SHA256
    "e7e7d761078f9a803de8d30a9f5dc836557f17278ebaf5583f3ad970caf853ce")

function(aipc_fetch_source name url sha256)
    string(TOUPPER "${name}" override_name)
    set(override_variable "FETCHCONTENT_SOURCE_DIR_${override_name}")
    if(AIPC_FETCHCONTENT_OFFLINE)
        if(NOT DEFINED ${override_variable} OR
           NOT IS_DIRECTORY "${${override_variable}}")
            message(FATAL_ERROR
                "Offline native build requires -D${override_variable}=<source-dir>")
        endif()
    endif()
    FetchContent_Declare(${name}
        URL "${url}"
        URL_HASH "SHA256=${sha256}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(${name})
    if(NOT ${name}_POPULATED)
        FetchContent_Populate(${name})
    endif()
    set(${name}_SOURCE_DIR "${${name}_SOURCE_DIR}" PARENT_SCOPE)
    set(${name}_BINARY_DIR "${${name}_BINARY_DIR}" PARENT_SCOPE)
endfunction()

function(aipc_add_lua lua_root)
    if(TARGET Aipc::Lua)
        return()
    endif()
    set(lua_sources
        lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c
        ldebug.c ldo.c ldump.c lfunc.c lgc.c linit.c liolib.c llex.c
        lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c loslib.c lparser.c
        lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c
        lutf8lib.c lvm.c lzio.c
    )
    list(TRANSFORM lua_sources PREPEND "${lua_root}/src/")
    add_library(aipc_lua STATIC ${lua_sources})
    add_library(Aipc::Lua ALIAS aipc_lua)
    target_include_directories(aipc_lua PUBLIC "${lua_root}/src")
    target_compile_definitions(aipc_lua PRIVATE LUA_USE_POSIX)
    target_link_libraries(aipc_lua PUBLIC m dl)
endfunction()

function(aipc_add_rockchip_targets rkmpi_root)
    if(TARGET Rockchip::MediaRuntime)
        return()
    endif()
    set(include_root "${rkmpi_root}/include")
    set(lib_root "${rkmpi_root}/lib/uclibc")
    foreach(required
            "${include_root}/rk_mpi_sys.h"
            "${lib_root}/libsample_comm.a"
            "${lib_root}/librockit.so"
            "${lib_root}/librockchip_mpp.so.0"
            "${lib_root}/librkaiq.so"
            "${lib_root}/librga.so")
        if(NOT EXISTS "${required}")
            message(FATAL_ERROR "Required Luckfox RKMPI artifact missing: ${required}")
        endif()
    endforeach()

    add_library(aipc_rockchip_headers INTERFACE)
    add_library(Rockchip::Headers ALIAS aipc_rockchip_headers)
    target_include_directories(aipc_rockchip_headers INTERFACE
        "${include_root}"
        "${include_root}/rkaiq"
        "${include_root}/rkaiq/uAPI2"
        "${include_root}/rkaiq/common"
        "${include_root}/rkaiq/xcore"
        "${include_root}/rkaiq/algos"
        "${include_root}/rkaiq/iq_parser"
        "${include_root}/rkaiq/iq_parser_v2"
    )

    foreach(spec
            "sample_comm;STATIC;libsample_comm.a"
            "rockit;SHARED;librockit.so"
            "mpp;SHARED;librockchip_mpp.so.0"
            "rkaiq;SHARED;librkaiq.so"
            "rga;SHARED;librga.so")
        list(GET spec 0 target_name)
        list(GET spec 1 target_type)
        list(GET spec 2 file_name)
        add_library(aipc_rockchip_${target_name} ${target_type} IMPORTED GLOBAL)
        set_target_properties(aipc_rockchip_${target_name} PROPERTIES
            IMPORTED_LOCATION "${lib_root}/${file_name}")
    endforeach()

    add_library(aipc_rockchip_media_runtime INTERFACE)
    add_library(Rockchip::MediaRuntime ALIAS aipc_rockchip_media_runtime)
    target_link_libraries(aipc_rockchip_media_runtime INTERFACE
        Rockchip::Headers
        aipc_rockchip_sample_comm
        aipc_rockchip_rockit
        aipc_rockchip_mpp
        aipc_rockchip_rkaiq
        aipc_rockchip_rga
    )
    target_link_directories(aipc_rockchip_media_runtime INTERFACE "${lib_root}")
    target_link_options(aipc_rockchip_media_runtime INTERFACE
        "-Wl,-rpath-link,${lib_root}")

    add_library(aipc_rockchip_visiong_runtime INTERFACE)
    add_library(Rockchip::VisionGRuntime ALIAS aipc_rockchip_visiong_runtime)
    target_link_libraries(aipc_rockchip_visiong_runtime INTERFACE Rockchip::Headers)
    target_link_directories(aipc_rockchip_visiong_runtime INTERFACE "${lib_root}")
    target_link_options(aipc_rockchip_visiong_runtime INTERFACE
        "-Wl,-rpath-link,${lib_root}")
    if(DEFINED AIPC_SDK_ROOT AND EXISTS "${AIPC_SDK_ROOT}/media/out/lib")
        target_link_directories(aipc_rockchip_visiong_runtime INTERFACE
            "${AIPC_SDK_ROOT}/media/out/lib")
        target_link_options(aipc_rockchip_visiong_runtime INTERFACE
            "-Wl,-rpath-link,${AIPC_SDK_ROOT}/media/out/lib")
    endif()
endfunction()

function(aipc_setup_dependencies)
    if(AIPC_FETCHCONTENT_OFFLINE)
        set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)
    endif()
    if(NOT DEFINED FETCHCONTENT_BASE_DIR)
        set(FETCHCONTENT_BASE_DIR
            "${CMAKE_CURRENT_SOURCE_DIR}/../target/native-deps" CACHE PATH
            "Shared FetchContent cache" FORCE)
    endif()

    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    set(JSON_Install OFF CACHE BOOL "" FORCE)
    aipc_fetch_source(nlohmann_json "${AIPC_NLOHMANN_JSON_URL}"
                      "${AIPC_NLOHMANN_JSON_SHA256}")
    if(NOT TARGET nlohmann_json::nlohmann_json)
        add_subdirectory("${nlohmann_json_SOURCE_DIR}"
                         "${nlohmann_json_BINARY_DIR}" EXCLUDE_FROM_ALL)
    endif()

    aipc_fetch_source(lua "${AIPC_LUA_URL}" "${AIPC_LUA_SHA256}")
    aipc_add_lua("${lua_SOURCE_DIR}")

    if(AIPC_ENABLE_RV1106)
        aipc_fetch_source(luckfox_rkmpi "${AIPC_LUCKFOX_RKMPI_URL}"
                          "${AIPC_LUCKFOX_RKMPI_SHA256}")
        aipc_add_rockchip_targets("${luckfox_rkmpi_SOURCE_DIR}")

        aipc_fetch_source(visiong "${AIPC_VISIONG_URL}" "${AIPC_VISIONG_SHA256}")
        set(VisionG_DIR "${visiong_SOURCE_DIR}/cmake" CACHE PATH "" FORCE)
        find_package(VisionG 1.2.1 REQUIRED CONFIG NO_DEFAULT_PATH)
        install(FILES "${visiong_SOURCE_DIR}/lib/libvisiong.so"
                DESTINATION lib)
        install(FILES
            "${visiong_SOURCE_DIR}/LICENSE"
            "${visiong_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            DESTINATION licenses/visiong)
        install(DIRECTORY "${visiong_SOURCE_DIR}/licenses/"
                DESTINATION licenses/visiong)
    endif()

    install(FILES "${lua_SOURCE_DIR}/doc/readme.html"
            DESTINATION licenses/lua)
endfunction()
