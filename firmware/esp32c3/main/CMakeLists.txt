idf_component_register(
    SRCS "app_main.cpp" "runtime_c3.cpp" "power_c3.cpp"
    INCLUDE_DIRS "."
    REQUIRES mcframe freertos esp_timer esp_system esp_pm)
