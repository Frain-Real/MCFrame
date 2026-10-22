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

`boards/f103_generic` и `boards/mock_generic` дают одинаковые имена
(`board::Uart`, `board::I2c`, `board::Spi`, `board::Pwm`, `board::Gpio`,
`board::Console`). На F103 USART1/I2C1 реальные, SPI/PWM пока
`NotSupported` — их реализации появятся в Phase 3.

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
