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
target_compile_definitions(mc_tests_stm32hal PRIVATE MCFRAME_STM32_REGS_FAKE=1)

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

    # Криптография считается дольше прочих: ECC — программная арифметика
    # по модулю, поэтому таймаут задан явно (mock-сборка идёт с ASan/UBSan).
    add_test(NAME crypto COMMAND mc_tests_crypto)
    set_tests_properties(crypto PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL"
        TIMEOUT 300)
endif()
