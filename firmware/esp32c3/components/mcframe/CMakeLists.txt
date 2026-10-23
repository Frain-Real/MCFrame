# MCFrame component for ESP32-C3.
# Планировщик MCFrame здесь НЕ компилируется: на C3 используется FreeRTOS
# (см. docs/adr/004-esp32c3.md). Компилируются shell, HAL C3 и BSP.
set(MCFRAME_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")

# littlefs: без malloc/assert, все буферы статические (LittleFs<...>).

idf_component_register(
    SRCS
        "${MCFRAME_ROOT}/src/os/shell/shell.cpp"
        "${MCFRAME_ROOT}/src/os/fs/lfs.c"
        "shell_c3.cpp"
        "${MCFRAME_ROOT}/src/boards/esp32c3/hal_esp32c3.cpp"
        "${MCFRAME_ROOT}/src/boards/esp32c3/board.cpp"
        "${MCFRAME_ROOT}/src/boards/esp32c3/wifi_esp.cpp"
        "${MCFRAME_ROOT}/src/boards/esp32c3/net_socket_esp.cpp"
        "${MCFRAME_ROOT}/src/boards/esp32c3/ble_esp.cpp"
        "${MCFRAME_ROOT}/src/boards/esp32c3/esp_partition_storage.cpp"
        "ota_https.cpp"
    INCLUDE_DIRS
        "${MCFRAME_ROOT}/src"
        "${MCFRAME_ROOT}/src/os/fs"
        "."
    REQUIRES
        esp_driver_gpio
        esp_driver_spi
        esp_driver_ledc
        esp_driver_uart
        nvs_flash
        esp_system
        freertos
        esp_https_ota
        esp_http_client
        mbedtls
        esp_wifi
        esp_netif
        esp_event
        lwip
        bt
        bootloader_support
        esp_partition
        app_update
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LFS_NO_MALLOC LFS_NO_ASSERT)
set_source_files_properties("${MCFRAME_ROOT}/src/os/fs/lfs.c"
    PROPERTIES COMPILE_OPTIONS "-w")


target_compile_definitions(${COMPONENT_LIB} PUBLIC
    MCFRAME_SOC_ESP32C3=1
    MCFRAME_KERNEL_FREERTOS=1
    MCFRAME_ARCH_RISCV32=1)
