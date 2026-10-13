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
    os/interfaces/dma_manager.cpp)

# ─────────── Платформенный слой ───────────
# platform_ops.cpp реализует контракт embed::platform::Ops — обязателен для
# ЛЮБОЙ платформы. platform.cpp содержит периферию (DMA/ADC) и присутствует
# только там, где она реально нужна.
set(OS_PLATFORM_SOURCES
    platforms/${MCU_TARGET}/platform_ops.cpp)

foreach(_extra platform.cpp platform_adc.cpp)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/platforms/${MCU_TARGET}/${_extra}")
        list(APPEND OS_PLATFORM_SOURCES platforms/${MCU_TARGET}/${_extra})
    endif()
endforeach()

# Тестовый интерфейс нужен только mock-сборке: он демонстрирует работу
# CRTP-интерфейсов и используется примером.
if(MCU_TARGET STREQUAL "mock")
    list(APPEND OS_PLATFORM_SOURCES
        platforms/mock/mock_interface.cpp
        platforms/mock/platform_adc.cpp)
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

add_library(MCFrame STATIC ${_core} ${OS_PLATFORM_SOURCES})
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
