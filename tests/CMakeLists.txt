# tests/CMakeLists.txt
# Тесты ядра запускаются только на mock-платформе: они используют
# платформенные заглушки (таймеры, критические секции) и не требуют железа.
if(NOT MCU_TARGET STREQUAL "mock")
    return()
endif()

add_executable(mc_tests test_os.cpp)
target_link_libraries(mc_tests PRIVATE MCFrame::MCFrame)
target_include_directories(mc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(BUILD_TESTING)
    add_test(NAME core_selftest COMMAND mc_tests)
    set_tests_properties(core_selftest PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")
endif()
