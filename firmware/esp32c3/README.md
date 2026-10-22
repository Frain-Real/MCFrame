# MCFrame на ESP32-C3

Phase 5: WiFi AP+STA (WPA2), BLE NimBLE (peripheral GATT, observer,
broadcaster), provisioning по BLE/UART и HTTPS OTA с TLS.

## Сборка

Нужен ESP-IDF 6.x (проверено на 6.0.2).

```bash
./firmware/esp32c3/build.sh
# или
cd firmware/esp32c3
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

В репозитории путь содержит пробел/кириллицу; `build.sh` копирует проект в
`/tmp/mcframe-c3-build` и собирает там.

## Разделы Flash (4 МБ)

| Раздел | Размер |
|---|---:|
| nvs | 16 КБ |
| otadata | 8 КБ |
| phy_init | 4 КБ |
| factory | 1 МБ |
| ota_0 | 1 МБ |
| ota_1 | 1 МБ |

## Shell

```text
help
reboot
wifi <ssid> <password>     # UART-provisioning (приоритетная задача)
ble scan|adv|stop
ota <https-url>            # HTTPS + TLS, запись в ota_0/ota_1
heap                       # свободная/минимальная куча
tasks                      # задачи, приоритеты, запас стека
reset                      # причина сброса (включая brown-out)
power                      # DFS/light sleep
gpio/spi/pwm ...
```

## BLE provisioning

GATT-сервис `6d636672-...`:
- SSID — write;
- Password — write;
- Command — write (`apply`);
- Status — read/notify.

После `apply` credentials сохраняются в NVS и WiFi переконфигурируется.

## Ограничения

- Central BLE: режим и connect() есть; GATT-клиент пока без API.
- Произвольный GATT-сервис через `add_gatt_service` пока `NotSupported`;
  встроенный provisioning-сервис работает.
- BLE pairing/security отключён, чтобы уложиться в 1 МБ.
- TLS 1.2, common CA bundle; TLS 1.3/IPv6 отключены.
- `mcframe_c3.bin` занимает ~0xee670 из 1 МБ (~72.0 КБ, 7% запаса).
- На реальной плате WiFi/BLE/OTA не проверялись.
