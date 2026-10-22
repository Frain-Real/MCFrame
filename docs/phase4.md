# Phase 4: ESP32-C3 без WiFi/BLE

## Сделано

- [x] IDF-проект `firmware/esp32c3`
- [x] компонент `components/mcframe`
- [x] FreeRTOS backend `os/kernel/backend_freertos/api.hpp`
- [x] статические задачи/очереди/мьютексы/семафоры/event groups
- [x] приоритетная политика: MCFrame 0..7 → FreeRTOS 12..5
- [x] C3 BSP: UART0, GPIO, SPI2, LEDC PWM, NVS
- [x] shell over UART0 (`help`, `reboot`, `gpio`, `spi`, `pwm`)
- [x] NVS init (`nvs_flash_init`)
- [x] task watchdog + `esp_timer` adapter
- [x] `sdkconfig.defaults`, `partitions.csv`, static allocation
- [x] статическая CI-проверка проекта

## Сборка

```bash
cd firmware/esp32c3
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Ограничения

- **IDF-сборка не запускалась в этом окружении**: ESP-IDF и
  `riscv32-esp-elf-gcc` отсутствуют. Проверен только статический состав
  проекта (`tools/check_project.py`).
- WiFi/BLE — Phase 5; `INetwork`/`IBle` пока не реализованы на C3.
- OTA-разделы/`esp_ota` — Phase 6.
- MCFrame `fault/watchdog/power` на C3 заменены ESP-IDF panic/task WDT/
  light sleep; адаптеры частичные.
- QEMU для ESP32-C3 не проверялся.
