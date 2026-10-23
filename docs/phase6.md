# Phase 6: NVS, OTA-инструменты, откат и питание

## Сделано

- [x] Портативный NVS `src/os/nvs`:
  - два банка и журнальная запись;
  - CRC32 заголовка банка и каждой записи;
  - wear leveling через чередование банков и компакцию;
  - атомарная компакция: записи пишутся до заголовка нового банка;
  - **v2**: namespaces, блобы до 512 Б, 64 ключа, schema version + миграция,
    опциональное AES-CTR+HMAC шифрование; старый формат читается и
    переписывается в v2.
- [x] `nvs::HalNvs` — адаптер `hal::INvs`.
- [x] Тест NVS: 74 проверки (namespaces, blobs, 64 ключа, миграция,
  шифрование, неверный ключ, power-fail).
- [x] F407/G431 `FlashOps`:
  - unlock/lock, erase sector/page, program, read;
  - F4 — 1 МБ, секторы 16/64/128 КБ;
  - G4 — 128 КБ, страницы 2 КБ, программирование 8 байт.
- [x] C3 `EspPartitionStorage` (ota::Storage) поверх `esp_partition`.
- [x] F407/G431 загрузчик и два слота:
  - F4: boot 16 КБ, meta 16 КБ, NVS 32 КБ, слоты по 448 КБ;
  - G4: boot 20 КБ, слоты по 50 КБ, meta 4 КБ, NVS 4 КБ;
  - `bootloader_fw`, `phase3_slot` (адрес слота A), VTOR;
  - `BootOps` F4/G4: проверка SP/вектора, VTOR, MSP, переход.
- [x] PVD/brown-out F1/F4/G4:
  - `power::Brownout::init(threshold_mv, cb, ctx)`, `below()`;
  - PVD EXTI16, порог 2.2–2.9 В, ISR-колбэк.
- [x] C3 OTA rollback:
  - `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`;
  - `esp_ota_mark_app_valid_cancel_rollback()` после успешной инициализации;
  - `esp_ota_mark_app_invalid_rollback_and_reboot()` при отказе shell/init.
- [x] `tools/mcflash.py`:
  - `pack`, `flash`, `verify`, `monitor`;
  - UART (pyserial), TCP и TLS (`--host`, `--tls`, `--cafile`);
  - SHA-256/CRC32, `--resume`, `--activate`, `--verify`.
- [x] Тесты mcflash: pack/unpack/коррупция/кадр.

## Проверка

```text
mock tests      28/28
NVS             74/74
mcflash tests   3/3
F4/G4 build     OK
F1 build        OK
C3 build        OK (0xeea90, слот 1.25 МБ, 0x51570 свободно, 25%)
```

## Ограничения

- **MOLP поверх TCP/TLS на устройстве** не подключён: mcflash умеет TCP/TLS,
  C3 использует HTTPS OTA через `esp_https_ota`, а не MOLP-сервер.
- OTA TLS на C3 привязан к **ISRG Root X1** (Let's Encrypt). Сервер с другим
  CA потребует заменить `isrg_root_x1.hpp`.
- NVS на STM32: G4 — 4 КБ (flat и slot); F1 flat — 4 КБ (образ 124 КБ);
  F1 slot — 0 (128 КБ заняты); F4 — meta 16 КБ (сектор 1), NVS 32 КБ (секторы
  2-3 по 16 КБ). Внешнее хранилище — через `hal::INvs`.
- C3 `mcframe_c3.bin` занимает 0xeea90 из 1.25 МБ (0x51570, ~25% запаса).
- Реального HIL нет: проверки NVS/Flash/OTA — на моделях и сборке, PVD —
  только сборка.
