cmake_minimum_required(VERSION 3.15)
project(RV1106_Edge_Smart_Network_Camera)
add_definitions(-DRV1106_1103)
set(CMAKE_CXX_STANDARD 17)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# opencv-mobile
set(OpenCV_DIR "../opencv-mobile-4.10.0-luckfox-pico/lib/cmake/opencv4")
find_package(OpenCV REQUIRED)
message(STATUS "Debug OpenCV_LIBS = ${OpenCV_LIBS}")

#Thread
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# 设置 RPATH
set(CMAKE_INSTALL_RPATH "/oem/usr/lib")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

# Collect source files
file(GLOB_RECURSE SOURCES 
    "main.cpp"
    "src/engine/*.cpp"
    "src/comm/*.cpp"
    "src/utils/*.cpp"
)

add_executable(${PROJECT_NAME} ${SOURCES})

add_compile_options(-g -Wall
        -DISP_HW_V30 -DRKPLATFORM=ON -DARCH64=OFF
        -DROCKIVA -DUAPI2
        -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64
)

target_include_directories(${PROJECT_NAME} PRIVATE
        ${OpenCV_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/engine
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/utils
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include/rknn
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include/librga
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include/rkaiq
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include/rkaiq/uAPI2
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include/rkaiq/common
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include/rkaiq/xcore
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include/rkaiq/algos
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include/rkaiq/iq_parser
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include/rkaiq/iq_parser_v2
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include/rkaiq/smartIr
)

target_link_directories(${PROJECT_NAME} PRIVATE
        ${OpenCV_LIB_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../common/lib/uclibc/
)

target_link_libraries(${PROJECT_NAME}
        ${OpenCV_LIBS}
        rknnmrt
        Threads::Threads
        rockiva
        sample_comm
        rockit
        rockchip_mpp
        rkaiq
        pthread
        rtsp
        rga
)

# Install
install(TARGETS ${PROJECT_NAME}
        DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/install
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/model
        DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/install
        DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
)
