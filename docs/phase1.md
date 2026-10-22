# Phase 1: критерии приёмки

## Сделано

- [x] `hal/IGpio.hpp`, `ISpi.hpp`, `IPwm.hpp`
- [x] `hal/IUart.hpp`, `II2c.hpp`, `IAdc.hpp`, `IFlash.hpp`
- [x] `hal/INetwork.hpp`, `IBle.hpp`, `INvs.hpp`
- [x] `hal/SpiDevice.hpp` — CS + блокировка шины
- [x] `os/kernel/kernel_api.hpp` + `backend_mcframe`
- [x] `boards/f103_generic/board.{hpp,cpp}`
- [x] `boards/mock_generic/board.{hpp,cpp}`
- [x] `board::Uart/I2c/Spi/Pwm/Gpio/Console`, `board::init()`
- [x] пример использует `board.hpp`, а не `uart_stm32.hpp`
- [x] mock-реализации HAL (`hal/mock/mock_hal.hpp`)
- [x] тест `tests/test_hal.cpp` — 64 проверки

## Проверка

```bash
./build.sh mock Debug
ctest --test-dir .build/mock --output-on-failure
.build/mock/tests/mc_tests_hal

./build.sh stm32 Release
tools/check_size.sh .build/stm32/example_fw.bin 131072
./build.sh stm32 Release -DMCFRAME_IMAGE=slot
tools/check_size.sh .build/stm32/example_fw.bin 53248
```

## Ограничения Phase 1

- F103: USART1/I2C1/LED PC13 реальные; SPI/PWM возвращают
  `NotSupported` до Phase 3.
- C3: `backend_freertos` намеренно `#error` до Phase 4.
- `board::spi1()`/`pwm1()` на F103 — заглушки, но API уже стабилен.
- Пример в slot-сборке не вызывает `board::init()`: минимальный образ не
  тратит байты на BSP.

## Дальше

Phase 2: ARMv7-M arch, FPU, F407/G431; Phase 3: GPIO/SPI/PWM STM32;
Phase 4: C3/FreeRTOS.
