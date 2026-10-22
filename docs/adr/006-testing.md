# ADR-006: тестирование без HIL

- Статус: Accepted
- Дата: 2026-10-22
- Контекст: Phase 0

## Ограничение

Доступа к HIL нет. CI не может проверить реальные GPIO/SPI/PWM, RF, токи и
латентность на плате. Это нужно явно учитывать в приёмке.

## Стратегия

1. **Unit + mock** — ядро, IPC, log/shell, crypto, OTA-протокол; ASan/UBSan.
2. **Host-модели регистров** — F1/F4/G4 (RCC/GPIO/SPI/TIM/DMA), как текущий
   `tests/host/stm32_regs_fake.*`.
3. **Interface mocks** — `MockSpi`, `MockGpio`, `MockNetwork`, `MockBle` для
   приложения и драйверов.
4. **QEMU/Renode** — ARMv7-M: загрузка, контекст, тики, Fault.
5. **ESP-IDF QEMU** — если поддерживает нужную ревизию C3; WiFi/BLE всё равно
   не эмулируются полноценно.
6. **Self-test firmware** — для каждой кастомной платы: GPIO loopback, SPI
   loopback, PWM измерение, UART echo. Владелец платы прогоняет вручную.

## CI

- mock Debug/Release;
- cross-build F103/F407/G431;
- ESP-IDF build для C3 (Phase 4);
- size budgets;
- fuzz smoke OTA/крипто;
- clang-tidy/cppcheck.

## Приёмка

- CI зелёный;
- self-test отчёт по каждой плате;
- RF/токи/латентность — только на железе, вне CI.
