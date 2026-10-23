# CMakeLists.txt — корень проекта MCFrame
cmake_minimum_required(VERSION 3.20)

# ─────────────────────────────────────────────────────────────────────────────
# Платформа выбирается toolchain-файлом, а НЕ настройкой CMAKE_C_COMPILER
# внутри этого файла: компиляторы и CMAKE_SYSTEM_NAME обязаны быть известны
# ДО project(), иначе CMake конфигурирует проект частично «по-хостяцки».
# ─────────────────────────────────────────────────────────────────────────────
set(MCU_TARGET "mock" CACHE STRING
    "Целевая платформа: mock | stm32 (F103) | stm32f4 | stm32g4 | esp32c3")
set_property(CACHE MCU_TARGET PROPERTY STRINGS
    mock stm32 stm32f4 stm32g4 esp32c3)

set(_toolchain "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-${MCU_TARGET}.cmake")
if(NOT EXISTS "${_toolchain}")
    message(FATAL_ERROR
        "Нет toolchain-файла для MCU_TARGET='${MCU_TARGET}': ${_toolchain}\n"
        "Доступны: mock, stm32, stm32f4, stm32g4, esp32c3")
endif()
set(CMAKE_TOOLCHAIN_FILE "${_toolchain}" CACHE FILEPATH "Toolchain" FORCE)

project(MCFrame VERSION 0.1.0 LANGUAGES C CXX ASM)

# Модель целей: arch × soc × board × kernel. Phase 0 регистрирует F407/G431/C3
# как stub-цели без прошивки (см. docs/adr/003-layer-model.md).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets.cmake)
mcframe_resolve_target()

if(MCFRAME_PHASE0_STUB)
    message(STATUS
        "MCFrame: Phase 0 stub для ${MCU_TARGET}/${MCFRAME_SOC}; "
        "прошивка появится в Phase 2/4")
    add_custom_target(mcframe_target_stub ALL
        COMMAND ${CMAKE_COMMAND} -E echo
            "MCFrame Phase 0: ${MCU_TARGET} arch=${MCFRAME_ARCH} soc=${MCFRAME_SOC} kernel=${MCFRAME_KERNEL} board=${MCFRAME_BOARD} — stub OK"
        COMMENT "Phase 0 target stub")
    return()
endif()

# Какой образ собирает пример: flat (вся Flash) или slot (слот A).
# Опция живёт здесь, а не только в example/: от неё зависит бюджет
# диагностики — см. MCFRAME_FAULT_REPORT ниже.
set(MCFRAME_IMAGE "flat" CACHE STRING "Образ example_fw: flat | slot")

# Политика загрузчика. Явная, а не «молчаливое» отключение SHA:
#   crc    — только CRC32 + вектор;
#   sha    — CRC32 + SHA-256 (если он записан);
#   signed — как sha + обязательный kRecFlagSignOk (подпись проверена OTA).
if(NOT DEFINED MCFRAME_BOOT_SECURITY)
    if(MCU_TARGET STREQUAL "stm32f4")
        set(MCFRAME_BOOT_SECURITY "crc" CACHE STRING
            "Политика загрузчика: crc | sha | signed (F4 по умолчанию crc: 16K)")
    else()
        set(MCFRAME_BOOT_SECURITY "sha" CACHE STRING
            "Политика загрузчика: crc | sha | signed")
    endif()
endif()
set_property(CACHE MCFRAME_BOOT_SECURITY PROPERTY STRINGS crc sha signed)
if(NOT MCFRAME_BOOT_SECURITY MATCHES "^(crc|sha|signed)$")
    message(FATAL_ERROR "MCFRAME_BOOT_SECURITY: ожидается crc|sha|signed, получено '${MCFRAME_BOOT_SECURITY}'")
endif()
if(MCFRAME_BOOT_SECURITY STREQUAL "crc")
    set(MCFRAME_BOOT_VERIFY_HASH 0)
    set(MCFRAME_BOOT_REQUIRE_SIGNATURE 0)
elseif(MCFRAME_BOOT_SECURITY STREQUAL "sha")
    set(MCFRAME_BOOT_VERIFY_HASH 1)
    set(MCFRAME_BOOT_REQUIRE_SIGNATURE 0)
else()
    set(MCFRAME_BOOT_VERIFY_HASH 1)
    set(MCFRAME_BOOT_REQUIRE_SIGNATURE 1)
endif()
message(STATUS "MCFrame: boot security=${MCFRAME_BOOT_SECURITY}")
if(MCU_TARGET STREQUAL "stm32f4")
    if(MCFRAME_BOOT_SECURITY STREQUAL "crc")
        message(STATUS "MCFrame: F4 boot использует CRC32 явно (16-КБ сектор); "
                       "SHA/signed: -DMCFRAME_BOOT_SECURITY=sha|signed")
    else()
        message(WARNING "MCFrame: F4 boot security=${MCFRAME_BOOT_SECURITY} "
                        "может не уложиться в 16 КБ; проверьте размер bootloader_fw")
    endif()
endif()
set_property(CACHE MCFRAME_IMAGE PROPERTY STRINGS flat slot)

# Квант round-robin для задач одного приоритета, тиков (0 — выключен).
# Включайте, если задачи равного приоритета не блокируются сами.
set(MCFRAME_TIME_SLICE_TICKS "0" CACHE STRING
    "Квант round-robin в тиках (0 = выключен)")

# Логи: сколько записей хранить и какой уровень компилировать.
set(MCFRAME_LOG_ENTRIES "8" CACHE STRING "Записей в кольце логов")
set(MCFRAME_LOG_LEVEL "3" CACHE STRING "Порог логов: 0 Error .. 3 Debug")

# Печатать отчёт об аварии (Fault/panic) через платформенный лог.
#
# По умолчанию ON. OFF экономит ~400 Б Flash: запись об аварии (.noinit +
# BKP) и перезагрузка остаются, печать — нет. Это нужно, если приложение
# переполняет слот 52 КБ; приложение может само вызвать fault::last().
if(NOT DEFINED MCFRAME_FAULT_REPORT)
    set(MCFRAME_FAULT_REPORT ON CACHE BOOL
        "Печатать отчёт об аварии (OFF экономит ~400 Б Flash)")
endif()
message(STATUS "MCFrame: отчёт об аварии=${MCFRAME_FAULT_REPORT}")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "" FORCE)
endif()

set(MCFRAME_RNDIS_RX_DEPTH "4" CACHE STRING "RNDIS RX packet queue depth")
set(MCFRAME_RNDIS_TX_DEPTH "4" CACHE STRING "RNDIS TX packet queue depth")
set(MCFRAME_RNDIS_FACTORY_SOCKETS "4" CACHE STRING "RNDIS net::Factory sockets")
set_property(CACHE MCFRAME_RNDIS_RX_DEPTH PROPERTY STRINGS 1 2 4 8 16)
set_property(CACHE MCFRAME_RNDIS_TX_DEPTH PROPERTY STRINGS 1 2 4 8 16)
set_property(CACHE MCFRAME_RNDIS_FACTORY_SOCKETS PROPERTY STRINGS 1 2 4 6 8)

option(MCFRAME_SANITIZE "ASan/UBSan для host-тестов (mock)" OFF)
option(MCFRAME_FUZZ "Собрать libFuzzer-цели (Clang)" OFF)
option(MCFRAME_COVERAGE "Инструментация покрытия host-тестов" OFF)
# mbedTLS: встроенный submodule third_party/mbedtls. Для F4/G4 включён по
# умолчанию; F1 не тянет (20 КБ RAM), C3 использует mbedTLS из ESP-IDF.
if(NOT DEFINED MCFRAME_MBEDTLS)
    set(_mcframe_tls_default OFF)
    if((MCU_TARGET STREQUAL "stm32f4" OR MCU_TARGET STREQUAL "stm32g4") AND
       EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/CMakeLists.txt")
        find_package(Python3 QUIET COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            execute_process(
                COMMAND "${Python3_EXECUTABLE}" -c "import jsonschema, jinja2"
                RESULT_VARIABLE _mcframe_tls_py_rc
                OUTPUT_QUIET ERROR_QUIET)
            if(_mcframe_tls_py_rc EQUAL 0)
                set(_mcframe_tls_default ON)
            else()
                message(STATUS "mbedTLS: Python deps missing, MCFRAME_MBEDTLS=OFF "
                               "(pip install -r requirements-mbedtls.txt)")
            endif()
        endif()
    endif()
    set(MCFRAME_MBEDTLS ${_mcframe_tls_default} CACHE BOOL
        "Build bundled mbedTLS (STM32 TLS)")
endif()
set(MCFRAME_MBEDTLS_INCLUDE_DIR "" CACHE PATH "внешний include-каталог mbedTLS (если submodule не используется)")
set(MCFRAME_MBEDTLS_LIBRARIES "" CACHE STRING "внешние mbedtls/mbedx509/tfpsacrypto libraries")
if(MCFRAME_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MCFRAME_FUZZ требует Clang/libFuzzer")
    endif()
    add_compile_options(-fsanitize=fuzzer-no-link,address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=fuzzer-no-link,address)
endif()

if(MCFRAME_COVERAGE)
    add_compile_options(--coverage -fno-omit-frame-pointer)
    add_link_options(--coverage)
endif()

if(MCFRAME_SANITIZE)
    if(NOT MCU_TARGET STREQUAL "mock")
        message(FATAL_ERROR "MCFRAME_SANITIZE поддерживается только для mock")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/mcu_flags.cmake)

# Compile definitions целевой матрицы для ядра и приложения:
# MCFRAME_ARCH_*, MCFRAME_SOC_*, MCFRAME_KERNEL_*, MCFRAME_BOARD_*.
mcframe_apply_target_definitions(mcu_flags)

# Раскладка Flash (загрузчик + слоты + метаданные) и функция подключения
# линкер-скрипта к конкретной прошивке. Один источник чисел на загрузчик,
# приложение и линкер — см. cmake/firmware_layout.cmake.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/firmware_layout.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rust.cmake)

# enable_testing() обязан вызываться в корневом CMakeLists до add_subdirectory,
# иначе ctest не увидит тесты подпроекта.
include(CTest)

add_subdirectory(src)

if(MCU_TARGET STREQUAL "stm32f4" OR MCU_TARGET STREQUAL "stm32g4")
    # Phase 2: пока только self-test ядра/FPU. Полное приложение — Phase 3.
    add_subdirectory(firmware/phase2)
    add_subdirectory(firmware/phase3)
    add_subdirectory(bootloader)
else()
    add_subdirectory(example)
    add_subdirectory(bootloader)
    add_subdirectory(tests)
endif()
