# 1. 指定目标系统信息
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# 2. 指定交叉编译器
set(TOOLCHAIN_PATH "/home/hao/projects/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf")
set(CMAKE_C_COMPILER "${TOOLCHAIN_PATH}/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PATH}/bin/arm-rockchip830-linux-uclibcgnueabihf-g++")

# 3. 指定 Sysroot (系统根目录)，让编译器和链接器能找到正确的头文件和库
set(CMAKE_SYSROOT "${TOOLCHAIN_PATH}/arm-rockchip830-linux-uclibcgnueabihf/sysroot")

# 告诉 CMake 不要为共享库链接创建依赖关系，这通常能避免生成不被支持的链接器标志
set(CMAKE_LINK_DEPENDS_NO_SHARED TRUE)

# 5. 配置查找库和头文件的默认路径
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)