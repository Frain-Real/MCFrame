# Phase 3: GPIO/SPI/PWM, BSP и board-команды

## Сделано

- [x] `hal::IGpioPin` + F1/F4/G4 GPIO (input/output/AF/analog, pull, speed)
- [x] EXTI для F1/F4/G4 (`attach_irq`/`detach_irq`, IRQ-обработчики в BSP)
- [x] `hal::ISpi` + F1/F4/G4 SPI (mode 0..3, baud, bit order)
- [x] F1 SPI DMA (DMA1 Ch2/3 через `DmaManager`)
- [x] F4/G4 SPI DMA (DMA2 Stream0/3 / DMA1 + DMAMUX)
- [x] SPI IRQ (F1/F4/G4) через `SpiConfig::irq`
- [x] `hal::IPwm` + F1/F4/G4 PWM (frequency/duty/start/stop)
- [x] `SpiDevice` + CS-полярность и блокировка шины
- [x] BSP `f103_generic`, `f407_generic`, `g431_generic`
- [x] F4/G4 UART1 (polling) в BSP
- [x] F4/G4 self-test `phase3_fw.elf` (GPIO/SPI/PWM)
- [x] shell-команды `gpio/spi/pwm` (F1 shell)
- [x] host-модели/тесты:
  - F1 GPIO/SPI/PWM на модели регистров — 14 проверок;
  - F4/G4 RCC/GPIO/SPI/TIM — 9 проверок.

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
- IRQ-обработчики EXTI/SPI живут в BSP-файлах: приложение, не использующее
  BSP, должно само определить их или линковать board.cpp.
- Shell-команды `gpio/spi/pwm` включены в полную (не minimal) сборку.
