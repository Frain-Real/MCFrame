# firmware/phase2/CMakeLists.txt
# Phase 2 self-test: ARMv7-M + FPU-контекст на F407/G431.
# Полное приложение (периферия) появится в Phase 3.
add_executable(phase2_fw
    phase2_main.cpp
    ${CMAKE_SOURCE_DIR}/src/platforms/${MCU_TARGET}/startup_${MCU_TARGET}.s
    ${CMAKE_SOURCE_DIR}/src/platforms/${MCU_TARGET}/system_${MCU_TARGET}.c)
target_link_libraries(phase2_fw PRIVATE MCFrame::MCFrame)

set(_linker "${CMAKE_SOURCE_DIR}/src/platforms/${MCU_TARGET}/linker.ld")
target_link_options(phase2_fw PRIVATE
    -T${_linker}
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/phase2_fw.map
    -nostartfiles
    -specs=nosys.specs)

set_target_properties(phase2_fw PROPERTIES
    OUTPUT_NAME phase2_fw
    SUFFIX ".elf")

# QEMU MPS2 AN386: тот же self-test, но код по адресу 0x00000000.
add_executable(phase2_qemu
    phase2_main.cpp
    ${CMAKE_SOURCE_DIR}/src/platforms/${MCU_TARGET}/startup_${MCU_TARGET}.s
    ${CMAKE_SOURCE_DIR}/src/platforms/${MCU_TARGET}/system_${MCU_TARGET}.c)
target_link_libraries(phase2_qemu PRIVATE MCFrame::MCFrame)
target_link_options(phase2_qemu PRIVATE
    -T${CMAKE_SOURCE_DIR}/src/platforms/stm32f4/linker_qemu.ld
    -Wl,--gc-sections
    -nostartfiles
    -specs=nosys.specs)
set_target_properties(phase2_qemu PROPERTIES SUFFIX ".elf")
