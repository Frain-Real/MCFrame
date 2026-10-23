# Phase 7: production hardening

## Сделано

- [x] C3 power management:
  - `esp_pm_configure`: DFS 160/40 МГц + light sleep;
  - `CONFIG_PM_ENABLE=y`;
  - shell `power`.
- [x] C3 brown-out detector: `CONFIG_ESP_BROWNOUT_DET=y`;
  shell `reset` показывает `ESP_RST_BROWNOUT`.
- [x] C3 BLE security:
  - `CONFIG_BT_NIMBLE_SECURITY_ENABLE=y`;
  - SC + legacy pairing;
  - provisioning GATT characteristics требуют encryption
    (`READ_ENC`/`WRITE_ENC`).
- [x] C3 diagnostics:
  - `heap` — free/min heap;
  - `tasks` — имена, приоритеты, high-water mark стека;
  - `reset` — причина сброса;
  - `power` — текущий PM-конфиг.
- [x] ASan/UBSan для host-тестов:
  - CMake `-DMCFRAME_SANITIZE=ON`;
  - CI job `sanitizers`; 30/30 tests под ASan+UBSan.
- [x] F4/G4 STOP + RTC wakeup и IWDG:
  - `power_m4.cpp` подключается приложением/phase3, загрузчик остаётся на
    слабых заглушках;
  - IWDG с выбором предделителя/RLR, RCC_CSR reset cause, backup-регистры;
  - STOP через RTC wakeup timer, восстановление часов после WFI.
- [x] Phase 6 closure:
  - F4/G4 bootloader + два слота;
  - PVD F1/F4/G4;
  - NVS: F4 32 КБ (секторы 2-3), G4 4 КБ, F1 flat 2 КБ + 2 КБ meta; F1 slot — внешняя;
  - BLE central connect.
- [x] C3 headroom:
  - TLS pinned to ISRG Root X1 вместо CA bundle;
  - logs level NONE;
  - `mcframe_c3.bin` = 0xf1a40; после переразметки слот 1.25 МБ, свободно 0x4e5c0 (~314 КБ, 25%).

## Проверка

```text
mock Debug        30/30
ASan/UBSan        30/30
NVS               74/74
mcflash            3/3
F1/F4/G4 builds   OK
C3 build          OK (0xf1a40, 25% free)
```

F4/G4 boot:

```text
F4 boot 16K, meta 16K, backup 16K, NVS 32K, slots 448K/384K
G4 boot 20K, slots 48K, meta 4K, backup 4K, NVS 4K
```

## Ограничения

- MOLP-over-TCP/TLS на устройстве по-прежнему не подключён: C3 использует
  HTTPS OTA, mcflash — TCP/TLS-клиент.
- BLE GATT-клиент (discovery/read/write) не обёрнут в API; central умеет
  подключаться.
- C3 TLS принимает только цепочки до ISRG Root X1.
- HIL нет: PVD, power/light sleep, BLE pairing и OTA проверены сборкой,
  а не платой.
- C3 logs выключены (`LOG_DEFAULT_LEVEL_NONE`) ради запаса Flash.
