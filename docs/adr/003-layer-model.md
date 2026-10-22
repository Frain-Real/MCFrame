# ADR-003: модель слоёв arch × soc × board × kernel

- Статус: Accepted
- Дата: 2026-10-22
- Контекст: Phase 0

## Проблема

Текущий `MCU_TARGET=mock|stm32` и папка `src/platforms/stm32` смешивают
архитектуру (ARMv7-M), SoC (STM32F1), плату (generic) и ядро. Для четырёх
целей это не масштабируется.

## Решение

```
src/
  os/          переносимое ядро и сервисы
  arch/        armv7m | riscv32
  hal/         публичные интерфейсы
  soc/         stm32/common, stm32/f1|f4|g4, esp32c3
  boards/      <board>: пины, тактирование, алиасы
  platforms/   mock (host)
```

Правила зависимостей:

- `os` не включает `soc`/`arch`/`boards`/vendor headers;
- `hal` не включает `soc`;
- `soc` реализует `hal` и использует `arch`;
- `boards` выбирает `soc` и задаёт пины/такты;
- `platform::Ops` — контракт ядра (критические секции, контекст, тик, память),
  не HAL приложения.

## Целевая матрица

| target | arch | soc | kernel | phase |
|---|---|---|---|---|
| mock | host | host | mcframe | done |
| stm32 | armv7m | stm32f1 | mcframe | done |
| stm32f4 | armv7m | stm32f4 | mcframe | Phase 2 |
| stm32g4 | armv7m | stm32g4 | mcframe | Phase 2 |
| esp32c3 | riscv32 | esp32c3 | freertos | Phase 4 |

CMake-опции: `MCU_TARGET`, `MCFRAME_ARCH`, `MCFRAME_SOC`, `MCFRAME_BOARD`,
`MCFRAME_KERNEL`, `MCFRAME_PHASE0_STUB`.

## Последствия

- Phase 0: F407/G431/C3 регистрируются как stub-цели без прошивки.
- Phase 1: появляются `hal/`, `boards/`, `os/kernel`.
- Phase 2/4: реальные SoC-порты.
