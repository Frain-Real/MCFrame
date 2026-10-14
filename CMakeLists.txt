# CMakeLists.txt — корень проекта MCFrame
cmake_minimum_required(VERSION 3.20)

# ─────────────────────────────────────────────────────────────────────────────
# Платформа выбирается toolchain-файлом, а НЕ настройкой CMAKE_C_COMPILER
# внутри этого файла: компиляторы и CMAKE_SYSTEM_NAME обязаны быть известны
# ДО project(), иначе CMake конфигурирует проект частично «по-хостяцки».
# ─────────────────────────────────────────────────────────────────────────────
set(MCU_TARGET "mock" CACHE STRING "Целевая платформа: mock | stm32")
set_property(CACHE MCU_TARGET PROPERTY STRINGS mock stm32)

set(_toolchain "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-${MCU_TARGET}.cmake")
if(NOT EXISTS "${_toolchain}")
    message(FATAL_ERROR
        "Нет toolchain-файла для MCU_TARGET='${MCU_TARGET}': ${_toolchain}\n"
        "Доступны: mock, stm32")
endif()
set(CMAKE_TOOLCHAIN_FILE "${_toolchain}" CACHE FILEPATH "Toolchain" FORCE)

project(MCFrame VERSION 0.1.0 LANGUAGES C CXX ASM)

message(STATUS "MCFrame: платформа=${MCU_TARGET}")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "" FORCE)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/mcu_flags.cmake)

# Раскладка Flash (загрузчик + слоты + метаданные) и функция подключения
# линкер-скрипта к конкретной прошивке. Один источник чисел на загрузчик,
# приложение и линкер — см. cmake/firmware_layout.cmake.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/firmware_layout.cmake)

# enable_testing() обязан вызываться в корневом CMakeLists до add_subdirectory,
# иначе ctest не увидит тесты подпроекта.
include(CTest)

add_subdirectory(src)
add_subdirectory(example)
add_subdirectory(bootloader)
add_subdirectory(tests)
