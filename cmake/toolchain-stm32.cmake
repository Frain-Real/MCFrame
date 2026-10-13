# cmake/toolchain-stm32.cmake — STM32F103RBTx (Cortex-M3, medium density).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)

# gcc-ar нужен, чтобы -flto работал для статической библиотеки
# (обычный ar не подключает LTO-плагин).
set(CMAKE_AR      arm-none-eabi-gcc-ar     CACHE FILEPATH "")
set(CMAKE_RANLIB  arm-none-eabi-gcc-ranlib CACHE FILEPATH "")
set(CMAKE_OBJCOPY arm-none-eabi-objcopy    CACHE FILEPATH "")
set(CMAKE_SIZE    arm-none-eabi-size       CACHE FILEPATH "")

# -DSTM32F10X_MD: medium density (128K/20K). Без него заголовок выбирает
# STM32F10X_LD и сборка platform.cpp падает на DMA2-символах.
# MCFRAME_TARGET_MCU используется примером, чтобы отличить прошивку от mock.
set(MCU_DEFINES -DSTM32F10X_MD -DMCFRAME_TARGET_MCU)

set(MCU_ARCH_FLAGS -mcpu=cortex-m3 -mthumb)

set(MCU_C_FLAGS
    -Wall -Wextra
    -ffunction-sections -fdata-sections -fmerge-all-constants
    -fno-common -fno-unwind-tables -fno-asynchronous-unwind-tables
    -O2 -g3)

set(MCU_CXX_FLAGS
    ${MCU_C_FLAGS}
    -fno-exceptions -fno-rtti
    # Убирает зависимость от __cxa_guard_acquire/release, которых нет
    # при -nostdlib.
    -fno-threadsafe-statics)

# Линкер-скрипт платформы. Абсолютный путь берём от каталога этого файла,
# чтобы значение было известно уже на этапе configure.
get_filename_component(_mcframe_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(MCU_LINKER_SCRIPT "${_mcframe_root}/src/platforms/stm32/linker.ld" CACHE FILEPATH
    "Линкер-скрипт платформы")

# ВАЖНО: -nostdlib здесь НЕЛЬЗЯ. Он отключает libc и libgcc, из-за чего
# пропадают memset/__libc_init_array/__aeabi_uldivmod (последний нужен для
# 64-битных делений в таймере). Нужный минимализм даёт nano.specs.
# ── Регионы памяти ОС (см. src/platforms/stm32/linker.ld) ──
#
# Значение 0 = регион не резервируется. По умолчанию:
#   куча   0    — если приложение не вызывает heap_alloc/stack_alloc,
#                 кучу держать незачем (все TCB и стеки статические);
#   пул    0    — то же для пула стеков;
#   MSP    1K   — здесь живут обработчики прерываний и main.
#
# Переопределение из командной строки:
#   cmake .. -DMCU_TARGET=stm32 -DMCU_OS_HEAP_BYTES=4096
set(MCU_OS_HEAP_BYTES       "512" CACHE STRING "Байт под внутреннюю кучу ОС")
set(MCU_OS_STACK_POOL_WORDS "0"  CACHE STRING "Слов под пул стеков задач")
set(MCU_MSP_STACK_BYTES     "1K" CACHE STRING "Байт под основной стек MSP")

set(MCU_LINK_OPTIONS
    --specs=nano.specs
    --specs=nosys.specs
    -Wl,--defsym=_OS_Heap_Size_override=${MCU_OS_HEAP_BYTES}
    -Wl,--defsym=_OS_StackPool_Size_override=${MCU_OS_STACK_POOL_WORDS}*4
    -Wl,--defsym=_Min_Stack_Size_override=${MCU_MSP_STACK_BYTES}
    -Wl,--gc-sections
    -Wl,--print-memory-usage
    -Wl,-Map=${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.map)
