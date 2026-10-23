# HAL-интерфейсы и board facade

Phase 1 вводит `hal::` — pure virtual интерфейсы, от которых зависит
приложение и драйверы устройств. Реализации лежат в `soc/` (Phase 3/4),
mock-реализации — в `hal/mock/`.

## Интерфейсы

| Файл | Что описывает |
|---|---|
| `hal/IGpio.hpp` | `IGpioPin`: режим, pull, set/get/toggle, IRQ |
| `hal/ISpi.hpp` | `ISpi`: mode/baud, full-duplex transfer, lock/unlock |
| `hal/IPwm.hpp` | `IPwm`: frequency/duty/start/stop |
| `hal/IUart.hpp` | `IUart`: configure/open/read/write/writeLine |
| `hal/II2c.hpp` | `II2c`: configure/write/read/lock |
| `hal/IAdc.hpp` | `IAdc`: configure/read |
| `hal/IFlash.hpp` | `IFlash`: page_size/read/write/erase |
| `hal/INvs.hpp` | `INvs`: key-value storage |
| `hal/INetwork.hpp` | `INetwork`: WiFi STA/AP/scan/events |
| `hal/IBle.hpp` | `IBle`: advertise/GATT service/notify/events |
| `hal/SpiDevice.hpp` | `SpiDevice`: CS + блокировка шины |

Правила:

- методы `noexcept`, возвращают `Status`;
- интерфейс не владеет объектом; объекты статические;
- никаких `std::function`, `std::string`, RTTI, исключений;
- виртуальные вызовы не используются в ISR;
- `NotSupported` — нормальный ответ, если SoC не умеет функцию.

## Board facade

Приложение включает только `board.hpp`:

```cpp
#include "board.hpp"

board::init();
board::led().set(true);

hal::ISpi& spi = board::spi1();
hal::IUart& con = board::console();
```

Платы: `f103_generic`, `f407_generic`, `g431_generic`, `mock_generic`,
`esp32c3`. На STM32 доступны `board::uart<1/2>()`, `board::spi<1/2>()`,
`board::i2c<1/2>()`, `board::adc1()`, `board::pwm1()`, `board::nvs()`.
Пины — не константы в драйвере, а [`PinMap`](../src/boards/pin_map.hpp):
карта платы проверяется `static_assert` на конфликты, BSP создаёт драйверы
из неё. На F103 USART1/2/3 (один async-автомат), SPI1/2, I2C1, ADC1, PWM TIM2_CH1 — реальные
драйверы; на F4/G4 — USART1/2, SPI1/2, I2C1/2, ADC1, PWM.

## Mock-реализации

`hal/mock/mock_hal.hpp`:

- `MockSpi` — loopback, счётчики lock/unlock;
- `MockGpioPin` — уровень, `trigger()` для IRQ;
- `MockPwm`, `MockUart`, `MockI2c`, `MockAdc`;
- `MockFlash`, `MockNvs`;
- `MockNetwork`, `MockBle` — состояние и события.

Они используются `tests/test_hal.cpp` и позволяют проверять прикладную логику
без платы.

## Kernel facade

`os/kernel/kernel_api.hpp` — публичный API задач/сна/примитивов. На STM32
backend — `backend_mcframe` (тонкие алиасы текущего планировщика), на C3
будет `backend_freertos` (Phase 4).
