# tests/CMakeLists.txt
# Тесты ядра запускаются только на mock-платформе: они используют
# платформенные заглушки (таймеры, критические секции) и не требуют железа.
if(NOT MCU_TARGET STREQUAL "mock")
    return()
endif()

add_executable(mc_tests test_os.cpp)
target_link_libraries(mc_tests PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(mc_tests_async test_async.cpp)
target_link_libraries(mc_tests_async PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_async PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# P0: запись об аварии, очередь ожидания, PI, таймаут, сторожевой маркер.
add_executable(mc_tests_fault test_fault.cpp)
target_link_libraries(mc_tests_fault PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_fault PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# P1: IPC, WorkQueue, log, shell, статистика и round-robin.
add_executable(mc_tests_ipc test_ipc.cpp)
target_link_libraries(mc_tests_ipc PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_ipc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Phase 1: HAL-интерфейсы, mock-реализации, SpiDevice, board facade, kernel API.
add_executable(mc_tests_hal test_hal.cpp)
target_link_libraries(mc_tests_hal PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_hal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Phase 2: host-модели регистров F4/G4 (пока без драйверов).
add_executable(mc_tests_f4regs host/test_stm32f4_regs.cpp)
target_link_libraries(mc_tests_f4regs PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_f4regs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/host)

# Phase 3: F1 GPIO/SPI/PWM на модели регистров.
add_executable(mc_tests_stm32periph
    host/test_stm32_periph.cpp
    host/stm32_regs_fake.cpp
    host/fake_dma.cpp)
target_link_libraries(mc_tests_stm32periph PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_stm32periph PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/host
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platforms/stm32)
target_compile_definitions(mc_tests_stm32periph PRIVATE
    MCFRAME_STM32_REGS_FAKE=1
    STM32F10X_MD=1)

add_executable(mc_tests_periph test_uart_i2c.cpp)
target_link_libraries(mc_tests_periph PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_periph PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platforms/${MCU_TARGET})

# Криптография: стандартные тестовые векторы (FIPS/NIST/RFC) и проверки на
# каждый исправленный дефект модуля.
add_executable(mc_tests_crypto test_crypto.cpp)
target_link_libraries(mc_tests_crypto PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_crypto PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# ─────────── HAL-логика STM32-драйверов: три стратегии на ПК ───────────
#
# Собираются НАСТОЯЩИЕ заголовки platforms/stm32 (uart_stm32.hpp,
# i2c_stm32.hpp), но вместо MMIO подставляется программная модель регистров
# (tests/host/stm32_regs_fake.*). Переключатель подмены — MCFRAME_STM32_REGS_FAKE,
# его читает ровно один файл: platforms/stm32/stm32_regs.hpp.
#
# Платформа сборки при этом остаётся mock: тесту нужны платформенные
# заглушки ядра (таймер, критические секции). Контракт platform_dma_*
# реализован моделью DMA1 в tests/host/fake_dma.cpp — в mock-сборке его
# больше никто не предоставляет.
add_executable(mc_tests_stm32hal
    host/test_stm32_hal.cpp
    host/stm32_regs_fake.cpp
    host/fake_dma.cpp)
target_link_libraries(mc_tests_stm32hal PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_stm32hal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/host
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platforms/stm32)
target_compile_definitions(mc_tests_stm32hal PRIVATE
    MCFRAME_STM32_REGS_FAKE=1
    STM32F10X_MD=1)


# ─────────── F1: USART2/USART3 тем же async-автоматом, что USART1 ───────────
add_executable(mc_tests_uart_multi
    host/test_uart_multi.cpp
    host/stm32_regs_fake.cpp
    host/fake_dma.cpp)
target_link_libraries(mc_tests_uart_multi PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_uart_multi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/host
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platforms/stm32)
target_compile_definitions(mc_tests_uart_multi PRIVATE
    MCFRAME_STM32_REGS_FAKE=1
    STM32F10X_MD=1)

# ─────────── Арбитр каналов DMA: очередь работ (без железа) ───────────
#
# Работы здесь — заглушки, поэтому проверяется именно арбитрация: ожидание
# занятого канала, параллельность разных каналов, FIFO, пара каналов разом,
# переполнение очереди, отмена и таймаут. Контракт platform_dma_* берётся у
# той же модели DMA1, что и в тестах HAL-логики.
add_executable(mc_tests_dma_arbiter
    host/test_dma_arbiter.cpp
    host/stm32_regs_fake.cpp
    host/fake_dma.cpp)
target_link_libraries(mc_tests_dma_arbiter PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_dma_arbiter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/host
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platforms/stm32)

# ─────────── Супервизор энергосбережения: сон и статистика нагрузки ───────────
#
# Политика сна проверяется на модели энергосбережения mock-платформы
# (src/platforms/mock/platform_power_mock.hpp): источник пробуждения можно
# «снять», будильник — сделать грубым, платформу — заставить отказать во
# входе. Именно эти состояния на железе приводят к «уснул и не проснулся».
add_executable(mc_tests_power test_power.cpp)
target_link_libraries(mc_tests_power PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_power PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platforms/${MCU_TARGET})

# ─────────── Энергосбережение STM32: STOP, RTC, EXTI, профиль частоты ───────────
#
# Собирается НАСТОЯЩИЙ power_stm32.cpp (тот же файл, что уходит в прошивку) с
# флагом MCFRAME_STM32_DEEP_SLEEP=1 и моделью регистров RTC/PWR/EXTI/FLASH/SCB/
# SysTick. Проверяется последовательность ухода в STOP: вооружённый будильник
# до остановки ядра, остановленный SysTick, снятые флаги, измеренное время сна,
# возвращённое тактирование.
add_executable(mc_tests_stm32power
    host/test_stm32_power.cpp
    host/stm32_regs_fake.cpp
    ../src/platforms/stm32/power_stm32.cpp)
target_link_libraries(mc_tests_stm32power PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_stm32power PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/host
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platforms/stm32)
target_compile_definitions(mc_tests_stm32power PRIVATE
    MCFRAME_STM32_REGS_FAKE=1
    MCFRAME_STM32_DEEP_SLEEP=1)

# ─────────── Сторожевой таймер: слежение за участниками и посмертная запись ───────────
#
# Политика сторожа проверяется на модели (src/platforms/mock/platform_watchdog_mock.hpp):
# модель НЕ перезагружает хост, а считает, сколько раз железо сработало бы, —
# поэтому проверяется именно решение сервиса (кто опоздал, когда прекратилось
# кормление, что записано в регистры, переживающие сброс) и согласование с
# глубоким сном (сон не должен быть длиннее срока сторожа).
add_executable(mc_tests_watchdog test_watchdog.cpp)
target_link_libraries(mc_tests_watchdog PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_watchdog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platforms/${MCU_TARGET})

# ─────────── Сторожевой таймер STM32: IWDG, BKP, причина сброса ───────────
#
# Собирается настоящий watchdog_stm32.cpp на модели регистров. Модель
# воспроизводит две вещи, на которых чаще всего ошибаются: запись PR/RLR без
# ключа 0x5555 теряется, а флаги PVU/RVU снимаются не мгновенно (значит, их
# надо ДОЖДАТЬСЯ). Проверяется и расчёт таймаута по разбросу LSI 30–60 кГц:
# обещанный срок обязан быть гарантированным минимумом.
add_executable(mc_tests_stm32watchdog
    host/test_stm32_watchdog.cpp
    host/stm32_regs_fake.cpp
    ../src/platforms/stm32/watchdog_stm32.cpp)
target_link_libraries(mc_tests_stm32watchdog PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_stm32watchdog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/host
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platforms/stm32)
target_compile_definitions(mc_tests_stm32watchdog PRIVATE MCFRAME_STM32_REGS_FAKE=1)


# ─────────── Декларативная карта пинов: конфликты на этапе компиляции ───────────
add_executable(mc_tests_board_pins test_board_pins.cpp)
target_link_libraries(mc_tests_board_pins PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_board_pins PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)


# ─────────── Типобезопасное время: Duration/TimePoint и округление ───────────
add_executable(mc_tests_time_units test_time_units.cpp)
target_link_libraries(mc_tests_time_units PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_time_units PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)


# ─────────── F4: маршруты DMA и карта пинов (без железа) ───────────
add_executable(mc_tests_m4_drivers test_m4_drivers.cpp)
target_link_libraries(mc_tests_m4_drivers PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_m4_drivers PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platforms/stm32f4g4)


# ─────────── OTA meta journal: append вместо стирания сектора ───────────
add_executable(mc_tests_ota_meta test_ota_meta.cpp)
target_link_libraries(mc_tests_ota_meta PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_ota_meta PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# ─────────── Phase 6: NVS, две копии банка, CRC, power-fail ───────────
add_executable(mc_tests_nvs test_nvs.cpp)
target_link_libraries(mc_tests_nvs PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_nvs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# ─────────── OTA: приём прошивки, проверка, продолжение, активация ───────────
#
# Собирается настоящий os/ota (тот же код, что уходит в прошивку) поверх
# mock-платформы: носитель — модель встроенной Flash (128 КБ, страница 1 КБ,
# запись только «в стёртое»), интерфейсы — транспорт в памяти и mock-UART.
# Тест содержит и ВТОРУЮ сторону протокола (хост-отправителя), поэтому
# проверяется спецификация целиком, а не только приёмник.
add_executable(mc_tests_ota test_ota.cpp)
target_link_libraries(mc_tests_ota PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_ota PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platforms/${MCU_TARGET})

# ─────────── Загрузчик: выбор слота, проверка образа, переход ───────────
#
# Собирается настоящий os/boot поверх mock-платформы: носитель — модель
# встроенной Flash, «прыжок» — модель (boot_mock.hpp), которая запоминает адрес,
# VTOR и запрет прерываний. Так решения загрузчика проверяются без платы, а
# «прыжок в мусор» проверяется на РЕАЛЬНЫХ байтах вектора в модели носителя.
add_executable(mc_tests_boot test_boot.cpp)
target_link_libraries(mc_tests_boot PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests_boot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platforms/${MCU_TARGET})

if(BUILD_TESTING)
    add_test(NAME core_selftest COMMAND mc_tests)
    set_tests_properties(core_selftest PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")

    add_test(NAME async_io COMMAND mc_tests_async)
    set_tests_properties(async_io PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")

    add_test(NAME uart_i2c COMMAND mc_tests_periph)
    set_tests_properties(uart_i2c PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")

    add_test(NAME stm32_hal_strategies COMMAND mc_tests_stm32hal)
    set_tests_properties(stm32_hal_strategies PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")

    add_test(NAME dma_arbiter COMMAND mc_tests_dma_arbiter)
    set_tests_properties(dma_arbiter PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")

    add_test(NAME power_supervisor COMMAND mc_tests_power)
    set_tests_properties(power_supervisor PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")

    add_test(NAME stm32_power COMMAND mc_tests_stm32power)
    set_tests_properties(stm32_power PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")

    add_test(NAME watchdog COMMAND mc_tests_watchdog)
    set_tests_properties(watchdog PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")

    add_test(NAME stm32_watchdog COMMAND mc_tests_stm32watchdog)
    set_tests_properties(stm32_watchdog PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")

    add_test(NAME boot COMMAND mc_tests_boot)
    set_tests_properties(boot PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")

    add_test(NAME nvs COMMAND mc_tests_nvs)
    set_tests_properties(nvs PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

    add_test(NAME ota COMMAND mc_tests_ota)
    set_tests_properties(ota PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")

    # Криптография считается дольше прочих: ECC — программная арифметика
    # по модулю, поэтому таймаут задан явно (mock-сборка идёт с ASan/UBSan).
    add_test(NAME crypto COMMAND mc_tests_crypto)
    set_tests_properties(crypto PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL"
        TIMEOUT 300)

    add_test(NAME fault_and_wait COMMAND mc_tests_fault)
    set_tests_properties(fault_and_wait PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")

    add_test(NAME ipc_shell COMMAND mc_tests_ipc)
    set_tests_properties(ipc_shell PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")

    add_test(NAME hal_interfaces COMMAND mc_tests_hal)
    set_tests_properties(hal_interfaces PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")

    add_test(NAME f4_regs COMMAND mc_tests_f4regs)
    set_tests_properties(f4_regs PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")

    add_test(NAME stm32_periph COMMAND mc_tests_stm32periph)

    add_test(NAME board_pins COMMAND mc_tests_board_pins)

    add_test(NAME time_units COMMAND mc_tests_time_units)

    add_test(NAME ota_meta COMMAND mc_tests_ota_meta)

    add_test(NAME uart_multi COMMAND mc_tests_uart_multi)
    set_tests_properties(uart_multi PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")

    add_test(NAME m4_drivers COMMAND mc_tests_m4_drivers)
    set_tests_properties(m4_drivers PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")
    set_tests_properties(time_units PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")
    set_tests_properties(board_pins PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")
    set_tests_properties(stm32_periph PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")
endif()
