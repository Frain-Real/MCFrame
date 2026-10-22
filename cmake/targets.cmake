# cmake/targets.cmake
# ---------------------------------------------------------------------------
# МОДЕЛЬ ЦЕЛЕЙ MCFrame: arch × soc × board × kernel.
#
# Один MCU_TARGET больше не описывает платформу: F103/F407/G431 — это разные
# SoC с общим ARMv7-M, а ESP32-C3 — RISC-V и FreeRTOS. Здесь целевая матрица
# проверяется ДО сборки, чтобы несовместимая комбинация падала с понятным
# сообщением, а не «ошибкой линковки в чужом линкер-скрипте».
#
# Phase 0: F103/mock собираются, F407/G431/C3 регистрируются как stub-цели.
# Реальные порты появятся в Phase 2/4 (см. docs/adr/003-layer-model.md).
# ---------------------------------------------------------------------------

function(mcframe_upper_ident out value)
    string(TOUPPER "${value}" _v)
    string(REGEX REPLACE "[^A-Z0-9]" "_" _v "${_v}")
    set(${out} "${_v}" PARENT_SCOPE)
endfunction()

function(mcframe_resolve_target)
    set(_target "${MCU_TARGET}")

    # Ожидаемые arch/soc/kernel для каждой цели. Это единственный источник
    # истины для валидации и для compile definitions.
    if(_target STREQUAL "mock")
        set(_arch "host")
        set(_soc  "host")
        set(_kernel "mcframe")
        set(_stub OFF)
    elseif(_target STREQUAL "stm32")
        set(_arch "armv7m")
        set(_soc  "stm32f1")
        set(_kernel "mcframe")
        set(_stub OFF)
    elseif(_target STREQUAL "stm32f4")
        set(_arch "armv7m")
        set(_soc  "stm32f4")
        set(_kernel "mcframe")
        set(_stub ON)
    elseif(_target STREQUAL "stm32g4")
        set(_arch "armv7m")
        set(_soc  "stm32g4")
        set(_kernel "mcframe")
        set(_stub ON)
    elseif(_target STREQUAL "esp32c3")
        set(_arch "riscv32")
        set(_soc  "esp32c3")
        # ADR-004: WiFi/BLE на C3 требуют ESP-IDF/FreeRTOS. Свой планировщик
        # на том же ядре несовместим с радио.
        set(_kernel "freertos")
        set(_stub ON)
    else()
        message(FATAL_ERROR
            "MCFrame: неизвестный MCU_TARGET='${_target}'.\n"
            "Доступны: mock, stm32 (F103), stm32f4, stm32g4, esp32c3.")
    endif()

    # Пользователь может задать явно — но только совместимое.
    if(DEFINED MCFRAME_ARCH AND NOT MCFRAME_ARCH STREQUAL "" AND
       NOT MCFRAME_ARCH STREQUAL "${_arch}")
        message(FATAL_ERROR
            "MCFrame: MCFRAME_ARCH='${MCFRAME_ARCH}' несовместим с "
            "MCU_TARGET='${_target}' (ожидается '${_arch}').")
    endif()
    if(DEFINED MCFRAME_SOC AND NOT MCFRAME_SOC STREQUAL "" AND
       NOT MCFRAME_SOC STREQUAL "${_soc}")
        message(FATAL_ERROR
            "MCFrame: MCFRAME_SOC='${MCFRAME_SOC}' несовместим с "
            "MCU_TARGET='${_target}' (ожидается '${_soc}').")
    endif()
    if(DEFINED MCFRAME_KERNEL AND NOT MCFRAME_KERNEL STREQUAL "" AND
       NOT MCFRAME_KERNEL STREQUAL "${_kernel}")
        message(FATAL_ERROR
            "MCFrame: MCFRAME_KERNEL='${MCFRAME_KERNEL}' несовместим с "
            "MCU_TARGET='${_target}' (ожидается '${_kernel}').\n"
            "C3 с WiFi/BLE использует FreeRTOS backend (ADR-004).")
    endif()

    set(MCFRAME_ARCH   "${_arch}"   CACHE STRING "Архитектура: host|armv7m|riscv32" FORCE)
    set(MCFRAME_SOC    "${_soc}"    CACHE STRING "SoC: host|stm32f1|stm32f4|stm32g4|esp32c3" FORCE)
    set(MCFRAME_KERNEL "${_kernel}" CACHE STRING "Ядро: mcframe|freertos" FORCE)

    if(NOT DEFINED MCFRAME_BOARD OR MCFRAME_BOARD STREQUAL "")
        set(MCFRAME_BOARD "generic" CACHE STRING
            "Плата/BSP: generic или имя boards/<board>" FORCE)
    endif()
    set(MCFRAME_BOARD "${MCFRAME_BOARD}" CACHE STRING
        "Плата/BSP: generic или имя boards/<board>" FORCE)
    if(NOT MCFRAME_BOARD STREQUAL "generic")
        message(STATUS
            "MCFrame: board='${MCFRAME_BOARD}' — BSP появится в Phase 1; "
            "пока используется generic-конфигурация")
    endif()

    if(NOT DEFINED MCFRAME_PHASE0_STUB)
        set(MCFRAME_PHASE0_STUB "${_stub}" CACHE BOOL
            "Phase 0: регистрировать цель без прошивки" FORCE)
    else()
        # Командная строка могла дать UNINITIALIZED; нормализуем тип, чтобы
        # CMakeCache и CI-grep видели BOOL.
        set(MCFRAME_PHASE0_STUB "${MCFRAME_PHASE0_STUB}" CACHE BOOL
            "Phase 0: регистрировать цель без прошивки" FORCE)
    endif()

    if(MCFRAME_PHASE0_STUB AND NOT ${_stub})
        message(FATAL_ERROR
            "MCFrame: MCFRAME_PHASE0_STUB=ON, но '${_target}' уже собирается.")
    endif()
    if(NOT MCFRAME_PHASE0_STUB AND ${_stub})
        message(FATAL_ERROR
            "MCFrame: '${_target}' появится в Phase 2/4. "
            "Для Phase 0 оставьте MCFRAME_PHASE0_STUB=ON.")
    endif()

    message(STATUS "MCFrame: target=${_target} arch=${MCFRAME_ARCH} "
                   "soc=${MCFRAME_SOC} board=${MCFRAME_BOARD} "
                   "kernel=${MCFRAME_KERNEL} stub=${MCFRAME_PHASE0_STUB}")
endfunction()

function(mcframe_apply_target_definitions target)
    mcframe_upper_ident(_a "${MCFRAME_ARCH}")
    mcframe_upper_ident(_s "${MCFRAME_SOC}")
    mcframe_upper_ident(_k "${MCFRAME_KERNEL}")
    mcframe_upper_ident(_b "${MCFRAME_BOARD}")
    target_compile_definitions(${target} INTERFACE
        MCFRAME_ARCH_${_a}=1
        MCFRAME_SOC_${_s}=1
        MCFRAME_KERNEL_${_k}=1
        MCFRAME_BOARD_${_b}=1)
    message(STATUS "MCFrame: defs ARCH_${_a} SOC_${_s} KERNEL_${_k} BOARD_${_b}")
endfunction()
