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

    # Shell над UART1 вместо приёма OTA. Два читателя одного UART не могут
    # делить MOLP-кадры, поэтому при ON транспорт OTA отключается.
    option(MCFRAME_APP_SHELL "Shell over UART1 in example (OTA transport off)" OFF)
    if(MCFRAME_APP_SHELL AND MCFRAME_IMAGE STREQUAL "slot")
        message(FATAL_ERROR
            "MCFrame: shell над UART1 не влезает в слот 52 КБ вместе с OTA. "
            "Соберите flat: ./build.sh stm32 Release -DMCFRAME_APP_SHELL=ON")
    endif()

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
    if(MCFRAME_APP_SHELL)
        target_compile_definitions(example_fw PRIVATE MCFRAME_APP_SHELL=1)
        message(STATUS "example_fw: shell over UART1 (OTA transport disabled)")
    endif()

    if(MCFRAME_APP_MINIMAL)
        target_compile_definitions(example_fw PRIVATE MCFRAME_APP_MINIMAL=1)
        # Слот 52 КБ: минимальному приложению важнее размер, чем -O2.
        # Отдельный INTERFACE-таргет добавлен ПОСЛЕ MCFrame, поэтому -Os
        # оказывается позже -O2 из mcu_flags (иначе -O2 победил бы).
        add_library(app_size_flags INTERFACE)
        target_compile_options(app_size_flags INTERFACE -Os)
        target_link_libraries(example_fw PRIVATE app_size_flags)
    else()
        target_compile_definitions(example_fw PRIVATE MCFRAME_APP_MINIMAL=0)
    endif()

    if(MCFRAME_RUST_APP AND TARGET mcframe_rust_app)
        add_dependencies(example_fw mcframe_rust_app)
        # Rust staticlib — ДО MCFrame: её объект ссылается на C ABI-мост
        # (mc_log/mc_sleep_ms), который лежит в libMCFrame.a. При обратном
        # порядке компоновщик уже прошёл архив и не вытащит мост.
        target_link_libraries(example_fw PRIVATE ${MCFRAME_RUST_STATICLIB})
        # C ABI-мост лежит в libMCFrame.a, а Rust-архив идёт после неё.
        # --undefined заставляет вытащить объект моста до обработки Rust.
        target_link_options(example_fw PRIVATE
            -Wl,--undefined=mc_log
            -Wl,--undefined=mc_sleep_ms
            -Wl,--undefined=mc_uptime_ms)
        target_compile_definitions(example_fw PRIVATE MCFRAME_RUST_APP=1)
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


# F4/G4 power/watchdog — отдельный TU только для приложения: загрузчик
# (16 КБ) остаётся на слабых заглушках и не тянет STOP/IWDG.
if(MCU_TARGET STREQUAL "stm32f4" OR MCU_TARGET STREQUAL "stm32g4")
    target_sources(example_fw PRIVATE
        ${CMAKE_SOURCE_DIR}/src/platforms/stm32f4g4/power_m4.cpp)
endif()
