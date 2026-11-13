################################################################################
# 根 CMakeLists.txt —— JoyStick1 总控（STM32F411CEU6 / Cortex-M4F）
#
# 分层：
#   cubemax/   CubeMX 生成的全部产物（HAL、Core、启动文件、链接脚本、工具链）
#   APP/       业务代码（AppManager + 各功能模块），编译成 APP_LIB 静态库
#   tools/     构建期用的小工具（头文件目录收集等）
#
# 构建：cmake -B build -G Ninja   然后  ninja -C build
################################################################################
cmake_minimum_required(VERSION 3.20)

# 1. 引用 cubemax 里的工具链文件（必须在 project() 之前）
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/cubemax/cmake/gcc-arm-none-eabi.cmake)

# 2. 工程基础配置
project(JoyStick1
    VERSION 1.0.0
    LANGUAGES C CXX ASM
)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 3. 全局编译警告（HAL 库自带不少无害警告，统一压掉，只留真正有用的告警）
add_compile_options(
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-but-set-variable>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-function>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-value>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-sign-compare>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-braces>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-pedantic>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>
    $<$<COMPILE_LANGUAGE:C,CXX>:-fdiagnostics-color=always>
    $<$<COMPILE_LANGUAGE:ASM>:-fdiagnostics-color=always>
)

# 4. 芯片级宏（全工程可见，包括 APP 业务代码）
add_compile_definitions(
    STM32F411xE       # STM32F411CEU6
    USE_HAL_DRIVER    # 启用 HAL 库
    __IO=volatile     # 兜底定义，防止 CMSIS 头文件没带进来
)

# 5. 子模块（顺序有讲究：APP 先建出 APP_LIB，cubemax 里的可执行目标才能链它）
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/cubemax/CMakeLists.txt)
    message(FATAL_ERROR "cubemax/CMakeLists.txt 不存在！")
endif()
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/APP/CMakeLists.txt)
    message(FATAL_ERROR "APP/CMakeLists.txt 不存在！")
endif()

add_subdirectory(APP)
add_subdirectory(cubemax)

# 6. 总构建目标：一次构建把固件和业务代码都编出来
#    APP_LIB 是 OBJECT 库（无 .a 产物），它挂在可执行目标上，所以这里只依赖 elf
#    提示信息只用 ASCII：Windows 控制台默认 GBK，中文 CMake message 会显示成乱码
add_custom_target(build_all ALL
    DEPENDS cubemax/JoyStick1.elf
    COMMAND ${CMAKE_COMMAND} -E echo "Build OK -> build/cubemax/JoyStick1.elf .hex .bin"
)

# 7. compile_commands.json 拷到根目录，供 clangd 索引（.vscode/.clangd 都指向这里）
add_custom_command(TARGET build_all POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_BINARY_DIR}/compile_commands.json
        ${CMAKE_SOURCE_DIR}/compile_commands.json
)
