# cmake/toolchain-mock.cmake — сборка и отладка на ПК.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(MCU_ARCH_FLAGS "")

set(MCU_C_FLAGS   -Wall -Wextra -Wshadow -Wconversion -Werror=return-type)
set(MCU_CXX_FLAGS -Wall -Wextra -Wshadow -Wconversion -Werror=return-type)
set(MCU_DEFINES   "")

# Санитайзеры ловят порчу памяти и UB, которые на MCU проявляются
# как «загадочные» HardFault.
set(MCFRAME_SANITIZE ON CACHE BOOL "Включить ASan/UBSan для mock-сборки")
if(MCFRAME_SANITIZE)
    list(APPEND MCU_C_FLAGS   -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer)
    list(APPEND MCU_CXX_FLAGS -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer)
    set(MCU_LINK_OPTIONS -fsanitize=address,undefined)
else()
    list(APPEND MCU_C_FLAGS   -g -O2)
    list(APPEND MCU_CXX_FLAGS -g -O2)
endif()
