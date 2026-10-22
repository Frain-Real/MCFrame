# Матрица целей MCFrame

| target | arch | soc | board | kernel | phase | сборка |
|---|---|---|---|---|---|---|
| `mock` | host | host | generic | mcframe | done | host-тесты |
| `stm32` | armv7m | stm32f1 | generic | mcframe | done | F103 |
| `stm32f4` | armv7m | stm32f4 | generic | mcframe | Phase 2 | stub |
| `stm32g4` | armv7m | stm32g4 | generic | mcframe | Phase 2 | stub |
| `esp32c3` | riscv32 | esp32c3 | generic | freertos | Phase 4 | stub |

## Команды

```bash
./build.sh mock Debug
./build.sh stm32 Release
./build.sh stm32f4 Release        # Phase 0 stub
./build.sh stm32g4 Release        # Phase 0 stub
python3 tools/target_plan.py      # проверка матрицы
```

## CMake-опции

- `MCU_TARGET` — `mock|stm32|stm32f4|stm32g4|esp32c3`;
- `MCFRAME_ARCH` — `host|armv7m|riscv32`;
- `MCFRAME_SOC` — `host|stm32f1|stm32f4|stm32g4|esp32c3`;
- `MCFRAME_BOARD` — `generic` или имя `boards/<board>`;
- `MCFRAME_KERNEL` — `mcframe|freertos`;
- `MCFRAME_PHASE0_STUB` — регистрация без прошивки.

Несовместимая комбинация падает на configure с понятным сообщением.

## Правила

- `esp32c3` требует `kernel=freertos` (ADR-004).
- `stm32f4/g4` пока stub; реальная прошивка — Phase 2.
- C3 stub может использовать host-fallback; реальная IDF-сборка требует ESP-IDF toolchain.
