# cmake/toolchain-stm32f4.cmake — STM32F407 (Cortex-M4F).
#
# Phase 0: цель зарегистрирована, реальный порт — Phase 2.
# FPU включён в toolchain, но контекстный переход обязан сохранять FPU
# (см. docs/adr/005-fpu.md), иначе hard-float запрещён.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_AR      arm-none-eabi-gcc-ar     CACHE FILEPATH "")
set(CMAKE_RANLIB  arm-none-eabi-gcc-ranlib CACHE FILEPATH "")
set(CMAKE_OBJCOPY arm-none-eabi-objcopy    CACHE FILEPATH "")
set(CMAKE_SIZE    arm-none-eabi-size       CACHE FILEPATH "")

set(MCU_ARCH_FLAGS -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
set(MCU_DEFINES -DSTM32F40_41xxx -DMCFRAME_TARGET_MCU -DMCFRAME_ARCH_FPU=1)
set(MCU_C_FLAGS
    -Wall -Wextra
    -ffunction-sections -fdata-sections -fmerge-all-constants
    -fno-common -fno-unwind-tables -fno-asynchronous-unwind-tables
    -O2 -g3)
set(MCU_CXX_FLAGS ${MCU_C_FLAGS} -fno-exceptions -fno-rtti -fno-threadsafe-statics)
set(MCU_LINK_OPTIONS "")
