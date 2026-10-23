# Phase 3: GPIO/SPI/PWM, BSP и board-команды

## Сделано

- [x] `hal::IGpioPin` + F1/F4/G4 GPIO (input/output/AF/analog, pull, speed)
- [x] EXTI для F1/F4/G4 (`attach_irq`/`detach_irq`, IRQ-обработчики в BSP)
- [x] `hal::ISpi` + F1/F4/G4 SPI (mode 0..3, baud, bit order)
- [x] F1 SPI DMA (DMA1 Ch2/3 через `DmaManager`)
- [x] F4/G4 SPI DMA через `dma_map_m4.hpp` (DMA2 Stream0/3 / DMA1 + DMAMUX)
- [x] SPI IRQ (F1/F4/G4) через `SpiConfig::irq`
- [x] `hal::IPwm` + F1/F4/G4 PWM (frequency/duty/start/stop)
- [x] `SpiDevice` + CS-полярность и блокировка шины
- [x] BSP `f103_generic`, `f407_generic`, `g431_generic`
- [x] F4/G4 UART1/2, I2C1/2, ADC1, SPI1/2 в BSP (polling; SPI — DMA)
- [x] F1 USART1/2/3 одним async-автоматом (Polling/Async/Dma), MD startup с USART3/SPI2
- [x] F4/G4 self-test `phase3_fw.elf` (GPIO/SPI/PWM); UART/I2C/ADC доступны через BSP
- [x] shell-команды `gpio/spi/pwm` (F1 shell)
- [x] host-модели/тесты:
  - F1 GPIO/SPI/PWM на модели регистров — 14 проверок;
  - F4/G4 RCC/GPIO/SPI/TIM — 9 проверок;
  - F4 DMA-маршруты и карта пинов — 14 проверок;
  - F1 USART2/3 (Polling/Async, NVIC, BRR, DMA routes) — 22 проверки.

## Проверка

```bash
./build.sh stm32 Release
./build.sh stm32 Release -DMCFRAME_APP_SHELL=ON
./build.sh stm32f4 Release
./build.sh stm32g4 Release
```

## Ограничения

- F4/G4 self-test проверяет конфигурацию/передачу, но не внешний loopback:
  без HIL это только сборка + проверка на плате.
- PWM-измерение и реальный SPI-loopback требуют осциллографа/платы.
- IRQ-обработчики EXTI живут в BSP-файлах: приложение, не использующее
  BSP, должно само определить их или линковать board.cpp.
- F4/G4 UART/I2C/ADC — блокирующие; ISR/DMA для них ещё не подключены.
- Shell-команды `gpio/spi/pwm` включены в полную (не minimal) сборку.
