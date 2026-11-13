################################################################################
# cubemax/CMakeLists.txt —— CubeMX 层：建立可执行目标，挂 HAL 源码，链业务库
# 说明：本文件由 CubeMX 原生根 CMakeLists 改造而来，可手动修改，CubeMX 不会覆盖
################################################################################
cmake_minimum_required(VERSION 3.22)

# 可执行目标名 = 根工程名 JoyStick1
# （cmake/stm32cubemx/CMakeLists.txt 用 ${CMAKE_PROJECT_NAME} 引用它，两者必须一致）
add_executable(${CMAKE_PROJECT_NAME})

# 挂载 CubeMX 生成的 HAL 配置、外设初始化与应用源码
add_subdirectory(cmake/stm32cubemx)

# 芯片级宏（stm32cubemx INTERFACE 库已带了，这里再兜底一次，给 objcopy/GDB 之外的场景）
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    STM32F411xE
    USE_HAL_DRIVER
    __IO=volatile
)

# CPU 架构选项（与 cubemax/cmake/gcc-arm-none-eabi.cmake 的 TARGET_FLAGS 保持一致）
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -mcpu=cortex-m4
    -mfpu=fpv4-sp-d16
    -mfloat-abi=hard
)

# 业务层静态库由 APP/CMakeLists.txt 产出（根 CMakeLists 先 add_subdirectory(APP)）
# 注意：CubeMX 的 cmake/stm32cubemx/CMakeLists.txt 用的是 plain 写法，
# 这里必须保持一致（plain），同一个 target 不能混用 keyword/plain
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
    APP_LIB
    m
)

# 工程里有 C++ 业务代码，统一用 g++ 收尾链接，保证 C++ 运行时符号能解析
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# ===================== 生成 bin/hex =====================
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.hex
    COMMENT "Generated ${CMAKE_PROJECT_NAME}.bin and ${CMAKE_PROJECT_NAME}.hex"
)
