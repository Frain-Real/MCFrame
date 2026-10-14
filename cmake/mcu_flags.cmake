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

# ──────────────────────────────────── LTO ────────────────────────────────────
#
# ЗАЧЕМ: без LTO каждая единица трансляции оптимизируется отдельно, поэтому
# функция, невидимая снаружи файла, остаётся настоящим вызовом — с сохранением
# регистров, выравниванием стека и без межмодульных упрощений. Для этого проекта
# цена выше обычной: ядро собирается в СТАТИЧЕСКУЮ БИБЛИОТЕКУ, то есть границы
# модулей (sheduler ↔ power ↔ watchdog ↔ драйверы) для оптимизатора такие же
# непрозрачные, как границы файлов, — а именно через них идёт горячий путь
# (тик, простой, проверка сроков).
#
# ПОЧЕМУ ОПЦИЯ, А НЕ «ВСЕГДА ВКЛЮЧЕНО»:
#   * LTO удлиняет сборку и требует, чтобы архиватор умел плагин LTO: индекс
#     символов LTO-объектов строит gcc-ar (в toolchain-файлах он и прописан),
#     обычный ar даёт «plugin needed to handle lto object»;
#   * при -O0 (Debug) LTO почти ничего не даёт, зато усложняет отладку
#     (инлайненный код, переставленные строки) — поэтому по умолчанию он
#     включён ТОЛЬКО для оптимизированных сборок;
#   * у LTO есть известные грабли с naked-функциями и таблицей векторов
#     (см. README, раздел «LTO»): их полезно включать осознанно.
#
# Режим задаётся ТРЁХЗНАЧНО: ON | OFF | AUTO.
#
# ПОЧЕМУ AUTO, А НЕ «BOOL ON ПО УМОЛЧАНИЮ ДЛЯ RELEASE»: значение по умолчанию,
# однажды положенное в кэш, остаётся там навсегда — и переконфигурирование уже
# существующего каталога сборки с Debug на Release молча оставляло бы LTO
# выключенным (проверено: именно так и произошло). AUTO пересчитывается при
# каждом запуске cmake, а явные ON/OFF пользователь задаёт сам и они, как и
# положено кэшу, запоминаются.
#
# Переопределение: -DMCFRAME_LTO=ON | -DMCFRAME_LTO=OFF | -DMCFRAME_LTO=AUTO
set(MCFRAME_LTO "AUTO" CACHE STRING
    "Межмодульная оптимизация: ON | OFF | AUTO (AUTO — для оптимизированных сборок)")
set_property(CACHE MCFRAME_LTO PROPERTY STRINGS AUTO ON OFF)

if(MCFRAME_LTO STREQUAL "AUTO")
    if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
        set(_mcframe_lto ON)
    else()
        set(_mcframe_lto OFF)
    endif()
else()
    set(_mcframe_lto "${MCFRAME_LTO}")
endif()

if(_mcframe_lto)
    # -flto=auto распараллеливает LTRANS через jobserver make: иначе на одном
    # ядре сборка прошивки заметно замедляется. Старые GCC такого варианта не
    # знают — тогда обычный -flto.
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-flto=auto" MCFRAME_FLTO_AUTO)
    if(MCFRAME_FLTO_AUTO)
        set(_mcframe_lto_flag "-flto=auto")
    else()
        set(_mcframe_lto_flag "-flto")
    endif()

    # Только C/C++: ассемблер (startup, core_cm3) LTO не касается, и лишний
    # флаг ему передавать не нужно.
    target_compile_options(mcu_flags INTERFACE
        $<$<COMPILE_LANGUAGE:C,CXX>:${_mcframe_lto_flag}>)
    # На линковке флаг обязателен: именно он запускает LTRANS и подключает
    # плагин, который читает LTO-объекты из архива.
    target_link_options(mcu_flags INTERFACE ${_mcframe_lto_flag})

    message(STATUS "MCFrame: LTO включён (${_mcframe_lto_flag}), запрошено ${MCFRAME_LTO}")
else()
    message(STATUS "MCFrame: LTO выключен (запрошено ${MCFRAME_LTO})")
endif()

