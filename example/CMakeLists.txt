# example/CMakeLists.txt
# ---------------------------------------------------------------------------
# Два исполняемых файла с РАЗНЫМИ toolchain-ами:
#   example_host — прогон логики ОС на ПК (только MCU_TARGET=mock)
#   example_fw   — прошивка для МКУ (только не-mock, с .bin/.hex и размером)
#
# Раньше оба создавались безусловно: при MCU_TARGET=stm32 host-цель
# собиралась arm-компилятором и падала на линковке с _fstat/_sbrk.
# ---------------------------------------------------------------------------

set(EXAMPLE_SOURCES app_main.cpp user_app.cpp)

if(MCU_TARGET STREQUAL "mock")
    # ─────────────────────── Host (mock) ───────────────────────
    add_executable(example_host ${EXAMPLE_SOURCES})
    target_link_libraries(example_host PRIVATE MCFrame::MCFrame)
    target_include_directories(example_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
else()
    # ─────────────────────── Прошивка ───────────────────────
    if(NOT MCU_LINKER_SCRIPT)
        message(FATAL_ERROR
            "MCU_LINKER_SCRIPT не задан: прошивку не собрать без линкер-скрипта")
    endif()

    add_executable(example_fw ${EXAMPLE_SOURCES})
    target_link_libraries(example_fw PRIVATE MCFrame::MCFrame)
    target_include_directories(example_fw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    if(CMAKE_OBJCOPY)
        add_custom_command(TARGET example_fw POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:example_fw>
                    ${CMAKE_BINARY_DIR}/example_fw.bin
            COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:example_fw>
                    ${CMAKE_BINARY_DIR}/example_fw.hex
            COMMENT "Генерация .bin и .hex")
    endif()
    if(CMAKE_SIZE)
        add_custom_command(TARGET example_fw POST_BUILD
            COMMAND ${CMAKE_SIZE} $<TARGET_FILE:example_fw>
            COMMENT "Размер прошивки")
    endif()
endif()
