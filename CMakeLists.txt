cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------------------
# 工具链设置(arm-none-eabi- 需在 PATH 中,本机随 STM32CubeCLT 安装)
# 必须在 project() 之前设置
# ---------------------------------------------------------------------------
set(CMAKE_SYSTEM_NAME               Generic)
set(CMAKE_SYSTEM_PROCESSOR          arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE   STATIC_LIBRARY)   # 交叉编译时不试链接完整程序

set(TOOLCHAIN_PREFIX                arm-none-eabi-)
set(CMAKE_C_COMPILER                ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER              ${CMAKE_C_COMPILER})
set(CMAKE_OBJCOPY                   ${TOOLCHAIN_PREFIX}objcopy)
set(CMAKE_SIZE                      ${TOOLCHAIN_PREFIX}size)

# ---------------------------------------------------------------------------
# 工程设置
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
set(CMAKE_PROJECT_NAME RTOS)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

project(${CMAKE_PROJECT_NAME} C ASM)
message("Build type: " ${CMAKE_BUILD_TYPE})

# ---------------------------------------------------------------------------
# CPU 与编译选项
# ---------------------------------------------------------------------------
set(CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard")

set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${CPU_FLAGS} -Wall -ffunction-sections -fdata-sections")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${CPU_FLAGS} -x assembler-with-cpp")
set(CMAKE_C_FLAGS_DEBUG   "-O0 -g3")
set(CMAKE_C_FLAGS_RELEASE "-Os -g0")

set(CMAKE_EXE_LINKER_FLAGS
    "${CPU_FLAGS} -T \"${CMAKE_SOURCE_DIR}/STM32F407xx_FLASH.ld\" --specs=nano.specs \
     -Wl,-Map=${CMAKE_PROJECT_NAME}.map -Wl,--gc-sections -Wl,--print-memory-usage")

# ---------------------------------------------------------------------------
# 源文件
# ---------------------------------------------------------------------------
add_executable(${CMAKE_PROJECT_NAME}
    main.c
    CMSIS/system_stm32f4xx.c
    startup_stm32f407xx.s
    RTOS/kernel.c
    RTOS/src/list.c
    RTOS/src/task.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    CMSIS
    RTOS
    RTOS/inc
)

# 只定义器件型号,CMSIS 头文件据此选择寄存器定义
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    STM32F407xx
)

# 链接完成后生成固件镜像并打印大小
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
    COMMENT "生成 RTOS.bin 并打印大小"
)
