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
  - F4: boot 16 КБ, meta 16 КБ, meta-backup 16 КБ (сектор 11), NVS 32 КБ,
    слот A 448 КБ, слот B 384 КБ; 16-КБ загрузчик по умолчанию использует
    политику CRC32 (явно, `MCFRAME_BOOT_SECURITY=crc`), SHA/signed включаются
    опцией и могут не влезть в 16 КБ;
  - G4: boot 20 КБ, слоты по 48 КБ, meta 4 КБ, backup 4 КБ, NVS 4 КБ;
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

## Power-loss компакция meta

`SlotStore`/`MetaJournal` поддерживают второй регион backup. Компакция:

1. пишет снимок актуальных записей в backup;
2. только после успешной записи стирает primary;
3. после успешной записи primary очищает backup.

Обрыв питания в любом окне оставляет либо primary, либо backup; при старте
`MetaJournal::scan()` при повреждённом primary читает backup. Тест
`tests/test_ota_meta.cpp` искусственно обрывает запись после `erase(primary)`.

F4: primary — сектор 1, backup — 16 КБ в секторе 11 (слот B уменьшен до 384 КБ).
G4: primary 4 КБ, backup 4 КБ, слоты по 48 КБ, NVS 4 КБ.
F1: backup выключен из-за нехватки места (`MCFRAME_META_BACKUP=0`); на F1
flat OTA нет, на F1 slot остаётся только append-журнал без backup.

## Политика безопасности загрузчика

Политика выбирается явно, а не «молчаливым» флагом:

```bash
-DMCFRAME_BOOT_SECURITY=crc     # CRC32 + вектор
-DMCFRAME_BOOT_SECURITY=sha     # CRC32 + SHA-256, если он записан
-DMCFRAME_BOOT_SECURITY=signed  # sha + обязательный kRecFlagSignOk
```

- `signed` не реализует криптографию в загрузчике: загрузчик доверяет только
  флагу, который OTA-приём выставил после `ota::Config::verify_signature`.
  Поэтому `signed` требует, чтобы приложение действительно настроило
  верификатор и `require_signature=true`;
- F4 по умолчанию `crc` из-за 16-КБ сектора; это **явно** печатается в
  configure. `sha`/`signed` можно включить, но нужно проверить размер;
- F1/G4 по умолчанию `sha`.

## Проверка

```text
mock tests      30/30
NVS             74/74
mcflash tests   3/3
F4/G4 build     OK
F1 build        OK
C3 build        OK (0xf24e0, слот 1.25 МБ, 0x4db20 свободно, 25%)
```

## Ограничения

- **MOLP поверх TCP/TLS на устройстве** не подключён: mcflash умеет TCP/TLS,
  C3 использует HTTPS OTA через `esp_https_ota`, а не MOLP-сервер.
- OTA TLS на C3 привязан к **ISRG Root X1** (Let's Encrypt). Сервер с другим
  CA потребует заменить `isrg_root_x1.hpp`.
- NVS на STM32: G4 — 4 КБ (flat и slot, слоты 48 КБ); F1 flat — 2 КБ NVS + 2 КБ meta
  (образ 124 КБ); F1 slot — 0 (128 КБ заняты); F4 — meta 16 КБ (сектор 1),
  backup 16 КБ (сектор 11), NVS 32 КБ (секторы 2-3 по 16 КБ). Внешнее хранилище — через `hal::INvs`.
- C3 `mcframe_c3.bin` занимает 0xf24e0 из 1.25 МБ (0x4db20, ~25% запаса).
- Реального HIL нет: проверки NVS/Flash/OTA — на моделях и сборке, PVD —
  только сборка.
