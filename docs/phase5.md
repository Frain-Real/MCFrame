# Phase 5: WiFi, BLE и HTTPS OTA на ESP32-C3

## Сделано

- [x] WiFi **AP+STA**, WPA2-PSK
- [x] `esp_wifi` + `esp_netif` + `esp_event`
- [x] scan, connect/disconnect, события через очередь в задачу
- [x] BLE NimBLE:
  - peripheral GATT server с provisioning-сервисом;
  - observer/scanning;
  - broadcaster;
  - GATT write callback, BLE events.
- [x] provisioning:
  - BLE: SSID/пароль/команда apply через GATT;
  - UART: `wifi <ssid> <password>`;
  - credentials в NVS;
  - shell-задача имеет приоритет выше остальных прикладных.
- [x] HTTPS OTA с TLS:
  - `esp_https_ota` + `esp_crt_bundle`;
  - разделы factory/ota_0/ota_1;
  - shell-команда `ota <https-url>`.
- [x] partition table:
  - nvs 16K, otadata 8K, phy_init 4K, factory/ota_0/ota_1 по 1.25M;
  - 192 КБ в конце Flash оставлены под будущий data-раздел.
- [x] сборка ESP-IDF 6.0.2 (реально выполнена).

## Сборка

```bash
./firmware/esp32c3/build.sh
# или
cd firmware/esp32c3
idf.py set-target esp32c3
idf.py build
```

В репозитории путь содержит пробел/кириллицу; `build.sh` при необходимости
копирует проект в `/tmp/mcframe-c3-build`.

## Размер

`mcframe_c3.bin` = 0xeea90 (977 552 Б). После переразметки слот — 1.25 МБ
(0x140000), поэтому запас вырос с ~7% до 25% (0x51570, ≈334 КБ). Дальнейшие фичи
больше не упираются в размер раздела.

## Ограничения

- **Central BLE**: реализован режим и подключение (`BleRole::Central`,
  `connect()`), но GATT-клиент (discovery/read/write) пока не обёрнут в API.
- GATT-сервис встроенный provisioning; произвольный `add_gatt_service` пока
  `NotSupported`.
- BLE security/pairing отключён (`CONFIG_BT_NIMBLE_SECURITY_ENABLE=n`),
  в ранней конфигурации; текущая Phase 7 включает pairing (см. phase7.md).
- TLS 1.2, общий bundle CA; TLS 1.3/DTLS отключены для размера.
- IPv6 отключён.
- Wi-Fi/BLE на реальной плате не проверялись (нет HIL).
- OTA HTTPS проверен сборкой, не реальным сервером.
