################################################################################
# APP/CMakeLists.txt —— 业务代码层：收集各功能模块，编成 APP_LIB 静态库
# 新增模块时只要建 APP/<模块名>/ 放 .cpp/.hpp，这里的 GLOB 会自动收进来
################################################################################
cmake_minimum_required(VERSION 3.20)

include(${CMAKE_SOURCE_DIR}/tools/cmake/FindInclude.cmake)

# 1. 收集业务源码
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/AppManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Joystick/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/JoyLink/*.cpp
)

# 2. 头文件路径：HAL/CMSIS 来自 cubemax 层，模块头文件目录自动收集
find_include_dir(APP_MODULE_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR})

set(APP_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/cubemax/Core/Inc
    ${CMAKE_SOURCE_DIR}/cubemax/Drivers/STM32F4xx_HAL_Driver/Inc
    ${CMAKE_SOURCE_DIR}/cubemax/Drivers/STM32F4xx_HAL_Driver/Inc/Legacy
    ${CMAKE_SOURCE_DIR}/cubemax/Drivers/CMSIS/Device/ST/STM32F4xx/Include
    ${CMAKE_SOURCE_DIR}/cubemax/Drivers/CMSIS/Include
    ${APP_MODULE_INCLUDE_DIRS}
)

# 3. 业务目标（根 CMakeLists 里先 add_subdirectory(APP)，cubemax 层再链接它）
#
# 用 OBJECT 库而不是 STATIC 库，原因：
#   CMake 生成静态库的规则是 "cmake -E rm -f 归档 && ar qc"。
#   本机有安全软件会异步拦截删除（cmake -E rm 返回成功但文件还在），
#   于是 ar qc 变成往旧归档里追加同名成员；ld 只取第一个同名成员，
#   结果就是改了代码、编出来的固件却还是旧逻辑——静默错误。
#   OBJECT 库不产生 .a、不调 ar，目标文件由 ninja 直接跟踪，改了必然重编、必然生效。
add_library(APP_LIB OBJECT ${APP_SOURCES})

target_include_directories(APP_LIB PUBLIC ${APP_INCLUDE_DIRS})

target_compile_definitions(APP_LIB PUBLIC
    STM32F411xE
    USE_HAL_DRIVER
)
