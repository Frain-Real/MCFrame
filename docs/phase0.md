# Phase 0: критерии приёмки

## Сделано

- [x] ADR-001..006
- [x] Матрица целей `docs/targets.md`
- [x] CMake-опции `MCFRAME_ARCH/SOC/BOARD/KERNEL`
- [x] Валидация несовместимых комбинаций
- [x] Toolchain-файлы F1/F4/G4/C3
- [x] Stub-цели F4/G4/C3
- [x] `tools/target_plan.py`
- [x] CI-заготовки для матрицы
- [x] F103/mock остаются зелёными

## Проверка

```bash
./build.sh mock Debug
ctest --test-dir .build/mock --output-on-failure

./build.sh stm32 Release
tools/check_size.sh .build/stm32/bootloader_fw.bin 20480
tools/check_size.sh .build/stm32/example_fw.bin 131072

./build.sh stm32f4 Release     # stub OK
./build.sh stm32g4 Release     # stub OK
python3 tools/target_plan.py   # матрица OK
```

## Вне Phase 0

- F407/G431: startup/linker/clock/FPU — Phase 2.
- C3: ESP-IDF/FreeRTOS backend — Phase 4.
- C3 stub: host-fallback при отсутствии RISC-V компилятора; реальная
  IDF-сборка — Phase 4.
- WiFi/BLE — Phase 5.
- GPIO/SPI/PWM — Phase 3/4.

## Приёмка

- F103/mock собираются и проходят тесты;
- F4/G4/C3 регистрируются без прошивки;
- несовместимая комбинация target/arch/soc/kernel падает на configure;
- CI содержит матрицу целей.
