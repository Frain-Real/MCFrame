# Документация MCFrame

- [Матрица целей](targets.md) — arch × soc × board × kernel.
- [Phase 0](phase0.md) — что сделано и как проверить.
- [Phase 1](phase1.md) — HAL, board, kernel facade.
- [Phase 2](phase2.md) — ARMv7-M, FPU, F407/G431.
- [Phase 3](phase3.md) — GPIO/SPI/PWM, EXTI, BSP, self-test.
- [Phase 4](phase4.md) — ESP32-C3, FreeRTOS backend, IDF-проект.
- [Phase 5](phase5.md) — WiFi/BLE, provisioning, HTTPS OTA.
- [Phase 6](phase6.md) — NVS, mcflash, rollback, FlashOps.
- [Phase 7](phase7.md) — power, security, diagnostics, sanitizers.
- [Phase 8](phase8.md) — F4/G4 BSP, NVS v2, Duration, декларативная плата, C3 repartition.
- [HAL и board](hal.md) — интерфейсы Phase 1.

## Architecture Decision Records

1. [ADR-001: kernel facade и два backend](adr/001-kernel-backends.md)
2. [ADR-002: HAL-интерфейсы](adr/002-hal-interfaces.md)
3. [ADR-003: слои arch/soc/board/kernel](adr/003-layer-model.md)
4. [ADR-004: ESP32-C3 на ESP-IDF/FreeRTOS](adr/004-esp32c3.md)
5. [ADR-005: hard-float и FPU-контекст](adr/005-fpu.md)
6. [ADR-006: тестирование без HIL](adr/006-testing.md)
