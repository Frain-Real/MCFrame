# Phase 2: ARMv7-M, FPU, F407/G431

## Сделано

- [x] `src/arch/armv7m/` — общий контекст ARMv7-M
- [x] F103 переведён на arch-порт
- [x] FPU-фрейм `S0–S15`, `S16–S31`, `FPSCR`, `CPACR`, `FPCCR`
- [x] `arch/armv7m/frame.hpp` + host-проверка раскладки (14 проверок)
- [x] host-модели RCC/GPIO/SPI/TIM F4/G4 (`tests/host/stm32f4_regs_fake.hpp`,
  9 проверок)
- [x] toolchain F407/G431 с hard-float
- [x] startup/linker/clock для F407/G431
- [x] минимальный platform::Ops
- [x] `firmware/phase2/phase2_main.cpp` — float-задачи и проверка
- [x] `phase2_fw.elf` для F407 и G431
- [x] QEMU-цель `phase2_qemu.elf` (ограничение: QEMU не моделирует FPU
  exception frame; на QEMU self-test не завершается)

## Проверка

```bash
./build.sh stm32f4 Release
arm-none-eabi-size .build/stm32f4/firmware/phase2/phase2_fw.elf

./build.sh stm32g4 Release
arm-none-eabi-size .build/stm32g4/firmware/phase2/phase2_fw.elf

./build.sh stm32 Release
tools/check_size.sh .build/stm32/example_fw.bin 131072
```

## Ограничения

- QEMU `mps2-an386`/`netduinoplus2` не воспроизводит FPU exception frame —
  FPU-контекст проверяется только сборкой + host-раскладкой; нужен стенд/плата.
- F407/G431 пока без GPIO/SPI/PWM/UART — Phase 3.
- `SystemInit` — HSI 16 МГц; PLL и периферийные такты — Phase 3.
- `power/watchdog/flash/boot` в Phase 2 — заглушки с `caps=0`.
