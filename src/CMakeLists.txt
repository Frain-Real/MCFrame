# src/CMakeLists.txt
# ---------------------------------------------------------------------------
# Явный список источников вместо file(GLOB ...): GLOB не отслеживает
# появление новых файлов, и добавленный модуль молча не попадает в сборку.
# ---------------------------------------------------------------------------

# ─────────── Ядро ОС (не зависит от платформы) ───────────
set(OS_CORE_SOURCES
    os/random.cpp
    os/fault/fault.cpp
    os/log/log.cpp
    os/shell/shell.cpp
    os/shell/shell_builtins.cpp
    os/core/memory/memory_manager.cpp
    os/core/memory/global_allocator.cpp
    os/core/task/sheduler.cpp
    os/power/power.cpp
    os/time/rtc.cpp
    os/rust/rust_bridge.cpp
    os/watchdog/watchdog.cpp
    os/interfaces/dma_manager.cpp
    os/interfaces/dma_arbiter.cpp
    os/fs/lfs.c)

# ─────────── Криптография (не зависит от платформы) ───────────
# Модуль целиком статический, без динамической памяти. Единицы трансляции
# нужны даже для шаблонных алгоритмов (AES/RSA/ECC/CRC): в них собраны явные
# инстанцирования, поэтому ошибки в константах и разрядности видны на этапе
# сборки библиотеки, а не при первом использовании в приложении.
# На размер прошивки это не влияет: статический архив линкуется по объектам,
# и неиспользованный алгоритм в неё просто не попадает.
set(OS_CRYPTO_SOURCES
    os/crypto/random.cpp
    os/crypto/crc.cpp
    os/crypto/sha2.cpp
    os/crypto/sha3.cpp
    os/crypto/hmac.cpp
    os/crypto/aes.cpp
    os/crypto/tdes.cpp
    os/crypto/rsa.cpp
    os/crypto/ecc.cpp
    os/crypto/ecies.cpp)

# ─────────── OTA: приём и проверка новой прошивки (не зависит от платформы) ───────────
# Модуль работает поверх двух абстракций: Storage (где лежит образ) и Transport
# (по какому интерфейсу приходит). Конкретику дают платформенные драйверы —
# поэтому одна и та же логика приёма, проверки и активации собирается и для
# STM32, и для стенда на ПК.
set(OS_OTA_SOURCES
    os/ota/ota.cpp)

# ─────────── Сеть: сокеты и протоколы MQTT/HTTP/SNTP ───────────
# Абстракция не зависит от lwIP/STM32: backend сокетов даёт платформа/борд.
set(OS_NET_SOURCES
    os/net/http.cpp
    os/net/sntp.cpp
    os/net/dhcp.cpp
    os/net/dns.cpp)

# ─────────── NVS: key-value поверх ota::Storage ───────────
set(OS_NVS_SOURCES
    os/nvs/nvs.cpp
    os/nvs/nvs_cipher.cpp)

# ─────────── Загрузчик: что грузить и когда передавать управление ───────────
# Политика (выбор слота, проверка образа, окно ожидания) не зависит от
# платформы; сам переход — контракт platform::BootOps (VTOR/MSP/обработчик
# сброса), поэтому на стенде он моделируется, а решения проверяются тестами.
# boot.cpp — политика (метки и переход), нужна и загрузчику, и приложению;
# boot_app.cpp — обвязка приложения, она работает через сервис OTA и в
# загрузчик не попадает (иначе он не влез бы в свой регион Flash).
set(OS_BOOT_SOURCES
    os/boot/boot.cpp
    os/boot/boot_app.cpp)

foreach(_f ${OS_CORE_SOURCES} ${OS_CRYPTO_SOURCES} ${OS_OTA_SOURCES} ${OS_NVS_SOURCES} ${OS_BOOT_SOURCES} ${OS_NET_SOURCES})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_f}")
        message(FATAL_ERROR "Нет источника ядра/криптографии/OTA: ${_f}")
    endif()
endforeach()

# ─────────── Платформенный слой ───────────
# platform_ops.cpp реализует контракт embed::platform::Ops — обязателен для
# ЛЮБОЙ платформы. platform.cpp содержит периферию (DMA/ADC) и присутствует
# только там, где она реально нужна.
if(MCU_TARGET STREQUAL "stm32f4" OR MCU_TARGET STREQUAL "stm32g4")
    set(OS_PLATFORM_SOURCES
        platforms/stm32f4g4/platform_ops.cpp
        platforms/stm32f4g4/rtc_m4.cpp
        platforms/stm32f4g4/usb_otg_fs.cpp)
    if(MCU_TARGET STREQUAL "stm32g4")
        list(APPEND OS_PLATFORM_SOURCES platforms/stm32g4/usb_fs_g4.cpp)
    endif()
else()
    set(OS_PLATFORM_SOURCES platforms/${MCU_TARGET}/platform_ops.cpp)
endif()

foreach(_extra platform.cpp platform_adc.cpp power_stm32.cpp watchdog_stm32.cpp
               brownout_stm32.cpp brownout_m4.cpp rtc_stm32.cpp rtc_m4.cpp
               flash_stm32.cpp flash_mock.cpp boot_stm32.cpp boot_mock.cpp)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/platforms/${MCU_TARGET}/${_extra}")
        list(APPEND OS_PLATFORM_SOURCES platforms/${MCU_TARGET}/${_extra})
    endif()
endforeach()

# Тестовый интерфейс нужен только mock-сборке: он демонстрирует работу
# CRTP-интерфейсов и используется примером.
if(MCU_TARGET STREQUAL "mock")
    list(APPEND OS_PLATFORM_SOURCES
        platforms/mock/mock_interface.cpp
        platforms/mock/platform_adc.cpp
        platforms/mock/platform_power.cpp
        platforms/mock/platform_watchdog.cpp)
endif()

# Startup-код и clock-инициализация нужны только для реального МКУ.
if(MCU_TARGET STREQUAL "stm32")
    list(APPEND OS_PLATFORM_SOURCES
        platforms/stm32/startup_stm32f10x_md.s
        platforms/stm32/cxx_runtime.cpp
        platforms/stm32/system_stm32f10x.c
        platforms/stm32/core_cm3.c
        platforms/stm32/exti_stm32.cpp
        arch/armv7m/context.cpp
        boards/f103_generic/board.cpp)
elseif(MCU_TARGET STREQUAL "mock")
    list(APPEND OS_PLATFORM_SOURCES
        boards/mock_generic/board.cpp)
elseif(MCU_TARGET STREQUAL "stm32f4" OR MCU_TARGET STREQUAL "stm32g4")
    if(MCU_TARGET STREQUAL "stm32f4")
        list(APPEND OS_PLATFORM_SOURCES boards/f407_generic/board.cpp)
    else()
        list(APPEND OS_PLATFORM_SOURCES boards/g431_generic/board.cpp)
    endif()
    list(APPEND OS_PLATFORM_SOURCES
        arch/armv7m/context.cpp
        platforms/stm32/cxx_runtime.cpp
        platforms/stm32f4g4/exti_m4.cpp
        platforms/stm32f4g4/periph_m4_instantiate.cpp)
endif()

foreach(_f ${OS_PLATFORM_SOURCES})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_f}")
        message(FATAL_ERROR "Нет источника платформы: ${_f}")
    endif()
endforeach()

# Опциональный глобальный аллокатор: переопределение malloc/free/new
# целиком — решение приложения, а не библиотеки. Иначе на mock-сборке
# libstdc++ (std::cout и т.п.) уходит в кучу ОС и падает.
option(MCFRAME_OVERRIDE_GLOBAL_ALLOCATOR
       "Перехватывать malloc/free/operator new кучей MCFrame" OFF)

set(_core ${OS_CORE_SOURCES})
if(NOT MCFRAME_OVERRIDE_GLOBAL_ALLOCATOR)
    list(REMOVE_ITEM _core os/core/memory/global_allocator.cpp)
endif()

# littlefs — сторонний C-код; его -Wconversion/-Wsign-conversion шумят
# в общем логе, не относясь к MCFrame. Ошибки при этом всё равно видны.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/os/fs/lfs.c"
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

add_library(MCFrame STATIC ${_core} ${OS_CRYPTO_SOURCES} ${OS_OTA_SOURCES}
                          ${OS_NVS_SOURCES} ${OS_BOOT_SOURCES} ${OS_NET_SOURCES}
                          ${OS_PLATFORM_SOURCES})
add_library(MCFrame::MCFrame ALIAS MCFrame)

# Приложение видит ТОЛЬКО ядро (os/...). Платформенные заголовки — PRIVATE:
# так приложение физически не может подключить, например,
# <mock_interface.hpp> из другого каталога платформы.
# platforms/<target> публикуется: приложению НУЖНЫ драйверы интерфейсов
# (uart_*.hpp, i2c_*.hpp), они header-only и содержат автоматы обмена.
# Заголовки ДРУГИХ платформ при этом недоступны — путь один.
target_include_directories(MCFrame
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/platforms/${MCU_TARGET}>
        $<INSTALL_INTERFACE:include>)

# Публичные требования: флаги через INTERFACE-таргет (устойчивы к внешним
# CMAKE_*_FLAGS) плюс include-путь к CMSIS у STM32.
target_link_libraries(MCFrame PUBLIC mcu_flags)

if(MCFRAME_MBEDTLS)
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/mbedtls/CMakeLists.txt")
        find_package(Python3 QUIET COMPONENTS Interpreter)
        if(NOT Python3_Interpreter_FOUND)
            message(FATAL_ERROR
                "mbedTLS 4.x требует Python3 для генерации файлов. "
                "Установите Python3 и задайте -DPython3_EXECUTABLE=...")
        endif()
        if(Python3_Interpreter_FOUND)
            execute_process(
                COMMAND "${Python3_EXECUTABLE}" -c "import jsonschema, jinja2"
                RESULT_VARIABLE _mbedtls_py_rc
                OUTPUT_QUIET ERROR_QUIET)
            if(NOT _mbedtls_py_rc EQUAL 0)
                message(FATAL_ERROR
                    "mbedTLS 4.x генератор требует Python-модули jsonschema и jinja2. "
                    "Установите: python3 -m pip install -r requirements-mbedtls.txt "
                    "и при необходимости задайте -DPython3_EXECUTABLE=...")
            endif()
        endif()
        if(MCU_TARGET STREQUAL "stm32f4" OR MCU_TARGET STREQUAL "stm32g4")
            set(MBEDTLS_CONFIG_FILE "${CMAKE_SOURCE_DIR}/cmake/mbedtls_config_stm32.h"
                CACHE FILEPATH "" FORCE)
            set(TF_PSA_CRYPTO_CONFIG_FILE
                "${CMAKE_SOURCE_DIR}/cmake/tf_psa_crypto_config_stm32.h"
                CACHE FILEPATH "" FORCE)
        endif()
        set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
        set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
        set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
        set(GEN_FILES ON CACHE BOOL "" FORCE)
        set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
        add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/mbedtls"
                         "${CMAKE_BINARY_DIR}/mbedtls" EXCLUDE_FROM_ALL)
        foreach(_tls_t mbedtls mbedx509 tfpsacrypto builtin everest p256m
                           platform utilities extras)
            if(TARGET ${_tls_t})
                target_link_libraries(${_tls_t} PRIVATE mcu_flags)
            endif()
        endforeach()
        if(MCU_TARGET STREQUAL "stm32f4" OR MCU_TARGET STREQUAL "stm32g4")
            add_library(mcframe_tls_platform STATIC
                "${CMAKE_SOURCE_DIR}/src/platforms/stm32f4g4/tls_platform_mbedtls.cpp")
            target_link_libraries(mcframe_tls_platform PRIVATE mcu_flags)
            target_include_directories(mcframe_tls_platform PRIVATE
                "${CMAKE_SOURCE_DIR}/src"
                "${CMAKE_SOURCE_DIR}/src/platforms/stm32f4g4"
                "${CMAKE_SOURCE_DIR}/third_party/mbedtls/include"
                "${CMAKE_SOURCE_DIR}/third_party/mbedtls/tf-psa-crypto/include"
                "${CMAKE_SOURCE_DIR}/third_party/mbedtls/tf-psa-crypto/drivers/builtin/include"
                "${CMAKE_BINARY_DIR}/mbedtls/include")
            target_compile_definitions(mcframe_tls_platform PRIVATE
                MCFRAME_WITH_MBEDTLS=1
                MBEDTLS_CONFIG_FILE="${CMAKE_SOURCE_DIR}/cmake/mbedtls_config_stm32.h"
                TF_PSA_CRYPTO_CONFIG_FILE="${CMAKE_SOURCE_DIR}/cmake/tf_psa_crypto_config_stm32.h")
        endif()
        if(TARGET mcframe_tls_platform)
            set(_mcframe_tls_platform mcframe_tls_platform)
        else()
            set(_mcframe_tls_platform "")
        endif()
        target_link_libraries(MCFrame PUBLIC
            $<BUILD_INTERFACE:mbedtls>
            $<BUILD_INTERFACE:mbedx509>
            $<BUILD_INTERFACE:tfpsacrypto>
            $<BUILD_INTERFACE:${_mcframe_tls_platform}>)
        add_library(mcframe_tls INTERFACE)
        add_library(MCFrame::Tls ALIAS mcframe_tls)
        target_link_libraries(mcframe_tls INTERFACE
            -Wl,--start-group
            MCFrame mbedtls mbedx509 tfpsacrypto ${_mcframe_tls_platform}
            -Wl,--end-group)
    else()
        if(MCFRAME_MBEDTLS_INCLUDE_DIR)
            target_include_directories(MCFrame PUBLIC ${MCFRAME_MBEDTLS_INCLUDE_DIR})
        endif()
        if(MCFRAME_MBEDTLS_LIBRARIES)
            target_link_libraries(MCFrame PUBLIC ${MCFRAME_MBEDTLS_LIBRARIES})
        endif()
    endif()
    target_compile_definitions(MCFrame PUBLIC MCFRAME_WITH_MBEDTLS=1)
endif()

if(MCU_TARGET STREQUAL "stm32f4" OR MCU_TARGET STREQUAL "stm32g4")
    target_include_directories(MCFrame PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/platforms/stm32f4g4>)
endif()

# Бюджет диагностики аварии: полный отчёт или «тихий» захват записи.
# Значение приходит из корневого CMakeLists (MCFRAME_FAULT_REPORT).
target_compile_definitions(MCFrame PRIVATE LFS_NO_MALLOC LFS_NO_ASSERT)
target_include_directories(MCFrame PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/os/fs>)
target_compile_definitions(MCFrame PUBLIC
    MCFRAME_FAULT_REPORT=$<BOOL:${MCFRAME_FAULT_REPORT}>
    MCFRAME_TIME_SLICE_TICKS=${MCFRAME_TIME_SLICE_TICKS}
    MCFRAME_LOG_ENTRIES=${MCFRAME_LOG_ENTRIES}
    MCFRAME_RNDIS_RX_DEPTH=${MCFRAME_RNDIS_RX_DEPTH}
    MCFRAME_RNDIS_TX_DEPTH=${MCFRAME_RNDIS_TX_DEPTH}
    MCFRAME_RNDIS_FACTORY_SOCKETS=${MCFRAME_RNDIS_FACTORY_SOCKETS}
    MCFRAME_LOG_LEVEL=${MCFRAME_LOG_LEVEL})

# F4-загрузчик обязан влезть в 16 КБ вместе с meta-журналом. SHA-256 образа
# проверяется при OTA-приёме; в загрузчике остаются CRC32 и проверка вектора.

# Область NVS в конце Flash (см. cmake/firmware_layout.cmake). 0 — плата
# использует внешнее хранилище или NVS не нужна.
if(DEFINED MCFRAME_NVS_SIZE)
    target_compile_definitions(MCFrame PUBLIC
        MCFRAME_NVS_OFFSET=${MCFRAME_NVS_OFFSET}
        MCFRAME_NVS_SIZE=${MCFRAME_NVS_SIZE})
endif()

# Политика загрузчика выбирается в корневом CMakeLists (crc/sha/signed),
# а не «молчаливым» отключением SHA.
target_compile_definitions(MCFrame PUBLIC
    MCFRAME_BOOT_VERIFY_HASH=${MCFRAME_BOOT_VERIFY_HASH}
    MCFRAME_BOOT_REQUIRE_SIGNATURE=${MCFRAME_BOOT_REQUIRE_SIGNATURE})
if(MCU_TARGET STREQUAL "stm32f4" OR MCU_TARGET STREQUAL "stm32g4" OR MCU_TARGET STREQUAL "mock")
    target_compile_definitions(MCFrame PUBLIC MCFRAME_META_BACKUP=1)
else()
    target_compile_definitions(MCFrame PUBLIC MCFRAME_META_BACKUP=0)
endif()
if(MCU_TARGET STREQUAL "stm32f4")
    # 16-КБ bootloader: таблица CRC32 (1 КБ) не помещается с запасом.
    target_compile_definitions(MCFrame PUBLIC MCFRAME_CRC_SMALL=1)
endif()

# CMSIS нужен и приложению: драйверы STM32 включают stm32f10x.h.
if(MCU_TARGET STREQUAL "stm32")
    target_include_directories(MCFrame PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/platforms/stm32>)
endif()

# ─────────── Установка для внешних проектов ───────────
include(GNUInstallDirs)
install(TARGETS MCFrame mcu_flags EXPORT MCFrameTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY os/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/os
        FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY hal/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hal
        FILES_MATCHING PATTERN "*.hpp")
install(FILES board.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY boards/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/boards
        FILES_MATCHING PATTERN "*.hpp")
install(EXPORT MCFrameTargets
        NAMESPACE MCFrame::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MCFrame)


if(MCFRAME_MBEDTLS)
    add_library(mcframe_tls_compile OBJECT
        "${CMAKE_SOURCE_DIR}/tests/tls_provider_compile.cpp")
    target_link_libraries(mcframe_tls_compile PRIVATE MCFrame)
    target_include_directories(mcframe_tls_compile PRIVATE
        "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src")
endif()
