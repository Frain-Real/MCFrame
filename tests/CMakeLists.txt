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
endif()
