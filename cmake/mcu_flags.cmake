# cmake/mcu_flags.cmake
# ---------------------------------------------------------------------------
# Флаги архитектуры и компилятора как INTERFACE-таргет.
#
# ПОЧЕМУ ТАК: переменные CMAKE_<LANG>_FLAGS_INIT действуют только как
# значение по умолчанию для кэша. Любой внешний -DCMAKE_C_FLAGS (CI, скрипт,
# IDE) МОЛЧА затирает их целиком, и сборка теряет -mcpu/-mthumb:
#   cmake .. -DMCU_TARGET=stm32 -DCMAKE_C_FLAGS="-DX"
#   -> C_FLAGS = -DX  -> ассемблер падает на 'mrs r3,psp' (ARM вместо Thumb)
# Флаги на target_*_options от этого защищены.
# ---------------------------------------------------------------------------

add_library(mcu_flags INTERFACE)

# Общие флаги (в т.ч. -mcpu/-mthumb): их потеря ломает всю сборку.
target_compile_options(mcu_flags INTERFACE
    $<$<COMPILE_LANGUAGE:C,CXX,ASM>:${MCU_ARCH_FLAGS}>)

# Флаги, специфичные для языка.
# -Wstrict-prototypes и -fno-rtti применимы только к одному языку,
# иначе GCC сыплет «valid for C but not for C++» / наоборот.
target_compile_options(mcu_flags INTERFACE
    $<$<COMPILE_LANGUAGE:C>:${MCU_C_FLAGS}>
    $<$<COMPILE_LANGUAGE:CXX>:${MCU_CXX_FLAGS}>
    $<$<COMPILE_LANGUAGE:ASM>:-x assembler-with-cpp>)

# Дефайны семейства (STM32F10X_MD и т.п.) — обязательны: без них
# stm32f10x.h выбирает STM32F10X_LD, где отсутствуют DMA2_Channel1_IRQn
# и RCC_AHBENR_DMA2EN.
target_compile_definitions(mcu_flags INTERFACE ${MCU_DEFINES})

# Опции линковки.
#
# КРИТИЧНО: MCU_ARCH_FLAGS обязательно должны попасть и в линковку.
# Иначе g++ линкует в ARM-режиме по умолчанию, а объекты собраны под Thumb,
# и линкер выдаёт:
#   "(__libc_init_array): Unknown destination type (ARM/Thumb)"
#   "dangerous relocation: unsupported relocation"
# Переменные MCU_* видны здесь, потому что файл включается через include()
# из корневого CMakeLists (та же область видимости).
target_link_options(mcu_flags INTERFACE
    ${MCU_ARCH_FLAGS}
    ${MCU_LINK_OPTIONS})

if(MCU_LINKER_SCRIPT)
    target_link_options(mcu_flags INTERFACE -T${MCU_LINKER_SCRIPT})
endif()
