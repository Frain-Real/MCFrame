# bootloader/CMakeLists.txt
# ---------------------------------------------------------------------------
# ЗАГРУЗЧИК: отдельный образ, отдельный адрес, отдельный линкер-скрипт.
#
#   bootloader_fw   — прошивка для МКУ: с 0x08000000, размер из раскладки
#                     (cmake/firmware_layout.cmake), .bin/.hex для прошивки
#   bootloader_host — прогон ЛОГИКИ загрузчика на ПК (mock-платформа): решение
#                     «что грузить» и переход моделируются, поэтому их можно
#                     проверить и показать без платы
#
# Почему отдельная цель, а не «режим» приложения: у загрузчика своя карта
# памяти, свой размер и своя точка входа. Собранный «тем же» образом он либо
# не влез бы в свой регион, либо перекрыл слот A.
# ---------------------------------------------------------------------------

set(BOOTLOADER_SOURCES boot_main.cpp)

if(MCU_TARGET STREQUAL "mock")
    # ─────────────────────── Стенд ───────────────────────
    add_executable(bootloader_host ${BOOTLOADER_SOURCES})
    target_link_libraries(bootloader_host PRIVATE MCFrame::MCFrame)
    target_include_directories(bootloader_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
else()
    # ─────────────────────── Прошивка ───────────────────────
    add_executable(bootloader_fw ${BOOTLOADER_SOURCES})
    if(MCU_TARGET STREQUAL "stm32f4" OR MCU_TARGET STREQUAL "stm32g4")
        target_sources(bootloader_fw PRIVATE
            ${CMAKE_SOURCE_DIR}/src/platforms/${MCU_TARGET}/startup_${MCU_TARGET}.s
            ${CMAKE_SOURCE_DIR}/src/platforms/${MCU_TARGET}/system_${MCU_TARGET}.c)
        target_link_options(bootloader_fw PRIVATE -nostartfiles -specs=nosys.specs)
    endif()
    target_link_libraries(bootloader_fw PRIVATE MCFrame::MCFrame)
    target_include_directories(bootloader_fw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # Образ загрузчика: адрес и размер — из общей раскладки.
    mcframe_firmware_bootloader(bootloader_fw)
    mcframe_flash_layout(bootloader_fw)

    # ── Принимает ли загрузчик прошивку сам ──
    #
    # ON  — в загрузчике живёт полный сервис OTA (~49 КБ на STM32F103): образ
    #       можно залить даже тогда, когда приложение не поднимается вовсе;
    # ON   требует MCFRAME_FLASH_BOOT_SIZE не меньше измеренного размера;
    # OFF — загрузчик только решает, проверяет и передаёт управление
    #       (~15 КБ): приём выполняет приложение. Так остаётся место на два
    #       слота по 44 КБ.
    #
    # Это НЕ две реализации приёма: при ON используется тот же ota::Service,
    # что и в приложении, — просто он вкомпонован в другой образ.
    option(MCFRAME_BOOTLOADER_OTA "Загрузчик принимает прошивку по OTA" OFF)
    if(MCFRAME_BOOTLOADER_OTA)
        target_compile_definitions(bootloader_fw PRIVATE MCFRAME_BOOTLOADER_OTA=1)
        message(STATUS "bootloader_fw: приём OTA включён (загрузчик больше)")
    else()
        target_compile_definitions(bootloader_fw PRIVATE MCFRAME_BOOTLOADER_OTA=0)
    endif()

    if(CMAKE_OBJCOPY)
        add_custom_command(TARGET bootloader_fw POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:bootloader_fw>
                    ${CMAKE_BINARY_DIR}/bootloader_fw.bin
            COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:bootloader_fw>
                    ${CMAKE_BINARY_DIR}/bootloader_fw.hex
            COMMENT "Генерация bootloader_fw.bin и .hex")
    endif()
    if(CMAKE_SIZE)
        add_custom_command(TARGET bootloader_fw POST_BUILD
            COMMAND ${CMAKE_SIZE} $<TARGET_FILE:bootloader_fw>
            COMMENT "Размер загрузчика")
    endif()
endif()
