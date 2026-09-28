add_executable(test_example main.cpp)

# Связываем пример с библиотекой myos
target_link_libraries(test_example PRIVATE MCFrame)
if(MCU_LINKER_SCRIPT)
    target_link_options(test_example PRIVATE -T${MCU_LINKER_SCRIPT})
    # Генерация .bin и .hex — опционально, но полезно
    add_custom_command(TARGET test_example POST_BUILD
        COMMAND arm-none-eabi-objcopy -O binary $<TARGET_FILE:test_example> ${CMAKE_BINARY_DIR}/test_example.bin
        COMMAND arm-none-eabi-objcopy -O ihex   $<TARGET_FILE:test_example> ${CMAKE_BINARY_DIR}/test_example.hex
        COMMENT "Generating .bin and .hex"
    )
endif()