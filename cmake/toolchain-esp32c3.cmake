# cmake/toolchain-esp32c3.cmake — ESP32-C3 (RISC-V RV32IMC).
#
# Phase 0: цель регистрируется как stub. Если настоящего RISC-V компилятора
# нет, для stub разрешён host-компилятор: исходников всё равно нет, а CI
# проверяет только целевую матрицу. В Phase 4 (MCFRAME_PHASE0_STUB=OFF)
# host-fallback запрещён и требуется riscv32-esp-elf-gcc.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(_mcframe_real_toolchain ON)
if(DEFINED MCFRAME_PHASE0_STUB AND NOT MCFRAME_PHASE0_STUB)
    set(_mcframe_real_toolchain ON)
elseif(NOT DEFINED MCFRAME_PHASE0_STUB)
    # По умолчанию Phase 0 stub; но если реальный компилятор есть — берём его.
    set(_mcframe_real_toolchain OFF)
else()
    set(_mcframe_real_toolchain OFF)
endif()

find_program(_mcframe_riscv_gcc riscv32-esp-elf-gcc)
if(_mcframe_riscv_gcc)
    set(CMAKE_C_COMPILER   riscv32-esp-elf-gcc)
    set(CMAKE_CXX_COMPILER riscv32-esp-elf-g++)
    set(CMAKE_ASM_COMPILER riscv32-esp-elf-gcc)
    set(CMAKE_AR      riscv32-esp-elf-ar      CACHE FILEPATH "")
    set(CMAKE_RANLIB  riscv32-esp-elf-ranlib  CACHE FILEPATH "")
    set(CMAKE_OBJCOPY riscv32-esp-elf-objcopy CACHE FILEPATH "")
    set(CMAKE_SIZE    riscv32-esp-elf-size    CACHE FILEPATH "")
elseif(_mcframe_real_toolchain)
    message(FATAL_ERROR
        "MCFrame: ESP32-C3 требует riscv32-esp-elf-gcc (ESP-IDF toolchain), "
        "но он не найден. Для Phase 0 используйте MCFRAME_PHASE0_STUB=ON.")
else()
    message(WARNING
        "MCFrame: Phase 0 C3 stub без riscv32-esp-elf-gcc — используется "
        "host-компилятор только для регистрации цели; прошивка не собирается.")
    set(CMAKE_C_COMPILER   gcc)
    set(CMAKE_CXX_COMPILER g++)
    set(CMAKE_ASM_COMPILER gcc)
    set(CMAKE_AR      ar      CACHE FILEPATH "")
    set(CMAKE_RANLIB  ranlib  CACHE FILEPATH "")
    set(CMAKE_OBJCOPY objcopy CACHE FILEPATH "")
    set(CMAKE_SIZE    size    CACHE FILEPATH "")
endif()

set(MCU_ARCH_FLAGS -march=rv32imc_zicsr_zifencei -mabi=ilp32)
set(MCU_DEFINES -DMCFRAME_TARGET_MCU -DMCFRAME_TARGET_ESP32C3)
set(MCU_C_FLAGS -Wall -Wextra -O2 -g3)
set(MCU_CXX_FLAGS ${MCU_C_FLAGS} -fno-exceptions -fno-rtti -fno-threadsafe-statics)
set(MCU_LINK_OPTIONS "")
