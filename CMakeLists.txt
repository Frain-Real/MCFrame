cmake_minimum_required(VERSION 3.20)

set(MCU_TARGET "mock" CACHE STRING "Target platform")

if(MCU_TARGET STREQUAL "mock")
    message(STATUS "Сборка для ПК (Mock)")
    set(CMAKE_C_COMPILER "gcc")
    set(CMAKE_CXX_COMPILER "g++")
    set(MCU_ARCH_FLAGS "") 
    set(MCU_LINKER_SCRIPT "")
    
elseif(MCU_TARGET STREQUAL "stm32")

    message(STATUS "Конфигурация: STM32 (arm-none-eabi)")

    set(CMAKE_SYSTEM_NAME Generic)
    set(CMAKE_SYSTEM_PROCESSOR arm)

    set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
    set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
    set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)

    # === СПИСОК флагов. Всё на одной строке внутри каждого элемента ===
    set(CPU_FLAGS
        -mcpu=cortex-m3
        -mthumb
        -Wall
        -Wstrict-prototypes
        -O2
        -g3
        -fmerge-all-constants
        -ffunction-sections
        -fdata-sections
        -fno-split-wide-types
        -fno-exceptions 
        -fno-rtti 
        -fno-common
        -fno-unwind-tables 
        -fno-asynchronous-unwind-tables
        -flto=auto
    )

    string(REPLACE ";" " " CPU_FLAGS_STR "${CPU_FLAGS}")

    set(CMAKE_C_FLAGS_INIT          "${CPU_FLAGS_STR}")
    set(CMAKE_CXX_FLAGS_INIT        "${CPU_FLAGS_STR}")
    set(CMAKE_ASM_FLAGS_INIT        "${CPU_FLAGS_STR} -x assembler-with-cpp")
    set(CMAKE_EXE_LINKER_FLAGS_INIT "${CPU_FLAGS_STR} -nostdlib -specs=nosys.specs -Wl,--relax,--gc-sections")

    set(CMAKE_C_COMPILER_WORKS   1)
    set(CMAKE_CXX_COMPILER_WORKS 1)
    set(CMAKE_ASM_COMPILER_WORKS 1)

    set(MCU_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/src/platforms/stm32/linker.ld")
    
elseif(MCU_TARGET STREQUAL "esp32")
    message(STATUS "Конфигурация: ESP32 (xtensa-esp32-elf)")

    set(CMAKE_SYSTEM_NAME Generic)
    set(CMAKE_SYSTEM_PROCESSOR xtensa)

    set(CMAKE_C_COMPILER   xtensa-esp32-elf-gcc)
    set(CMAKE_CXX_COMPILER xtensa-esp32-elf-g++)
    set(CMAKE_ASM_COMPILER xtensa-esp32-elf-gcc)

    # === Флаги для Xtensa ESP32 ===
    set(CPU_FLAGS
        -mlongcalls                    # КРИТИЧНО: вызовы функций дальше 4KB
        -mtext-section-literals        # Литералы в .text секции
        -Wall
        -Wstrict-prototypes
        -O2
        -g3
        -fmerge-all-constants
        -ffunction-sections
        -fdata-sections
        -fno-split-wide-types
        -fno-exceptions 
        -fno-rtti 
        -fno-common
        -fno-unwind-tables 
        -fno-asynchronous-unwind-tables
        -flto=auto
    )

    string(REPLACE ";" " " CPU_FLAGS_STR "${CPU_FLAGS}")

    set(CMAKE_C_FLAGS_INIT          "${CPU_FLAGS_STR}")
    set(CMAKE_CXX_FLAGS_INIT        "${CPU_FLAGS_STR}")
    set(CMAKE_ASM_FLAGS_INIT        "${CPU_FLAGS_STR} -x assembler-with-cpp")
    set(CMAKE_EXE_LINKER_FLAGS_INIT "${CPU_FLAGS_STR} -nostdlib -specs=nosys.specs -Wl,--gc-sections")

    set(CMAKE_C_COMPILER_WORKS   1)
    set(CMAKE_CXX_COMPILER_WORKS 1)
    set(CMAKE_ASM_COMPILER_WORKS 1)

    set(MCU_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/src/platforms/esp32/linker.ld")
    
else()
    message(FATAL_ERROR "Неизвестная платформа: ${MCU_TARGET}. Доступны: mock, stm32, esp32")
endif()

project(MCFrame C CXX ASM)

# Применяем флаги архитектуры глобально
add_compile_options(${MCU_ARCH_FLAGS})

# Подключаем подпроекты
add_subdirectory(src)
#add_subdirectory(example)