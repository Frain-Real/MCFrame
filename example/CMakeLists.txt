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
    add_executable(example_fw ${EXAMPLE_SOURCES})
    target_link_libraries(example_fw PRIVATE MCFrame::MCFrame)
    target_include_directories(example_fw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # ── Какой образ собираем: обычный или «приложение в слоте» ──
    #
    # flat — вся Flash с 0x08000000 (обновления по OTA нет);
    # slot — приложение по адресу слота A схемы с загрузчиком: тогда
    #        прошивка обязана переставить таблицу векторов на свой адрес
    #        (VECT_TAB_OFFSET), иначе первое прерывание уйдёт в загрузчик.
    set(MCFRAME_IMAGE "flat" CACHE STRING "Образ example_fw: flat | slot")
    set_property(CACHE MCFRAME_IMAGE PROPERTY STRINGS flat slot)

    # ── «Минимальное» приложение ──
    #
    # Полная «витрина» примера (АЦП, async-ввод-вывод с UART и I2C, отчёты) в
    # слот на 50 КБ не влезает. При сборке в слот демонстрации выключаются
    # автоматически (явное значение настройки при этом уважается).
    if(NOT DEFINED MCFRAME_APP_MINIMAL)
        if(MCFRAME_IMAGE STREQUAL "slot")
            set(MCFRAME_APP_MINIMAL ON)
            message(STATUS "example_fw: сборка в слот — демонстрации выключены "
                           "(MCFRAME_APP_MINIMAL=ON)")
        else()
            set(MCFRAME_APP_MINIMAL OFF)
        endif()
    endif()
    if(MCFRAME_APP_MINIMAL)
        target_compile_definitions(example_fw PRIVATE MCFRAME_APP_MINIMAL=1)
    else()
        target_compile_definitions(example_fw PRIVATE MCFRAME_APP_MINIMAL=0)
    endif()

    if(MCFRAME_IMAGE STREQUAL "slot")
        mcframe_firmware_slot(example_fw 0)
        # ВАЖНО: таблицу векторов приложение переставляет САМО, во время
        # выполнения (boot::relocate_vectors_to_self в начале main). Через
        # -DVECT_TAB_OFFSET этого сделать НЕЛЬЗЯ: system_stm32f10x.c собирается
        # в библиотеку один раз для всех образов, и «смещение на образ» до него
        # не доходит.
        message(STATUS "example_fw: приложение в слоте A (${MCFRAME_FLASH_SLOT0_OFFSET} Б от начала Flash)")
    else()
        mcframe_firmware_flat(example_fw)
    endif()

    mcframe_flash_layout(example_fw)

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
