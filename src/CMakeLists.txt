# src/CMakeLists.txt
# ---------------------------------------------------------------------------
# Явный список источников вместо file(GLOB ...): GLOB не отслеживает
# появление новых файлов, и добавленный модуль молча не попадает в сборку.
# ---------------------------------------------------------------------------

# ─────────── Ядро ОС (не зависит от платформы) ───────────
set(OS_CORE_SOURCES
    os/random.cpp
    os/core/memory/memory_manager.cpp
    os/core/memory/global_allocator.cpp
    os/core/task/sheduler.cpp
    os/power/power.cpp
    os/watchdog/watchdog.cpp
    os/interfaces/dma_manager.cpp
    os/interfaces/dma_arbiter.cpp)

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

foreach(_f ${OS_CORE_SOURCES} ${OS_CRYPTO_SOURCES} ${OS_OTA_SOURCES} ${OS_BOOT_SOURCES})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_f}")
        message(FATAL_ERROR "Нет источника ядра/криптографии/OTA: ${_f}")
    endif()
endforeach()

# ─────────── Платформенный слой ───────────
# platform_ops.cpp реализует контракт embed::platform::Ops — обязателен для
# ЛЮБОЙ платформы. platform.cpp содержит периферию (DMA/ADC) и присутствует
# только там, где она реально нужна.
set(OS_PLATFORM_SOURCES
    platforms/${MCU_TARGET}/platform_ops.cpp)

foreach(_extra platform.cpp platform_adc.cpp power_stm32.cpp watchdog_stm32.cpp
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
        platforms/stm32/startup_stm32f10x_ld.s
        platforms/stm32/system_stm32f10x.c
        platforms/stm32/core_cm3.c)
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

add_library(MCFrame STATIC ${_core} ${OS_CRYPTO_SOURCES} ${OS_OTA_SOURCES}
                          ${OS_BOOT_SOURCES} ${OS_PLATFORM_SOURCES})
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
install(EXPORT MCFrameTargets
        NAMESPACE MCFrame::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MCFrame)
