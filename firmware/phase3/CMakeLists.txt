# Phase 3 self-test: board GPIO/SPI/PWM на F407/G431.
add_executable(phase3_fw
    phase3_main.cpp
    ${CMAKE_SOURCE_DIR}/src/platforms/${MCU_TARGET}/startup_${MCU_TARGET}.s
    ${CMAKE_SOURCE_DIR}/src/platforms/${MCU_TARGET}/system_${MCU_TARGET}.c)
target_link_libraries(phase3_fw PRIVATE MCFrame::MCFrame)
target_link_options(phase3_fw PRIVATE
    -T${CMAKE_SOURCE_DIR}/src/platforms/${MCU_TARGET}/linker.ld
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/phase3_fw.map
    -nostartfiles
    -specs=nosys.specs)
set_target_properties(phase3_fw PROPERTIES SUFFIX ".elf")

if(MCU_TARGET STREQUAL "stm32f4" OR MCU_TARGET STREQUAL "stm32g4")
    add_executable(phase3_slot
        phase3_main.cpp
        ${CMAKE_SOURCE_DIR}/src/platforms/${MCU_TARGET}/startup_${MCU_TARGET}.s
        ${CMAKE_SOURCE_DIR}/src/platforms/${MCU_TARGET}/system_${MCU_TARGET}.c)
    target_link_libraries(phase3_slot PRIVATE MCFrame::MCFrame)
    mcframe_firmware_slot(phase3_slot 0)
    mcframe_flash_layout(phase3_slot)
    target_compile_definitions(phase3_slot PRIVATE VECT_TAB_OFFSET=${MCFRAME_FLASH_SLOT0_OFFSET})
    target_link_options(phase3_slot PRIVATE
        -Wl,--gc-sections
        -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/phase3_slot.map
        -nostartfiles
        -specs=nosys.specs)
    set_target_properties(phase3_slot PROPERTIES SUFFIX ".elf")
endif()
