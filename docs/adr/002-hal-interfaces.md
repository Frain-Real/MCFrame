# ADR-002: HAL-интерфейсы (наследование от pure virtual)

- Статус: Accepted
- Дата: 2026-10-22
- Контекст: Phase 0

## Проблема

Приложение должно работать с GPIO/SPI/PWM/UART/WiFi/BLE одинаково на
STM32F103/F407/G431 и ESP32-C3. Сейчас пример включает `uart_stm32.hpp` и
`i2c_stm32.hpp` напрямую; SPI/GPIO/PWM вообще отсутствуют как публичный API.

## Решение

Ввести `hal::` — набор pure virtual интерфейсов. Приложение зависит только от
них и от `board.hpp`; SoC-реализации лежат в `soc/`.

```cpp
namespace hal {
class ISpi {
public:
    virtual ~ISpi() = default;
    virtual Status configure(const SpiConfig&) noexcept = 0;
    virtual Status transfer(const void* tx, void* rx, Size n, Timeout t) noexcept = 0;
    virtual Status lock(uint32_t ticks) noexcept = 0;
    virtual void   unlock() noexcept = 0;
    virtual uint32_t caps() const noexcept = 0;
};
}
```

Интерфейсы 1.0:

- `IGpioPin` / `IGpioPort` — режим, pull, speed, set/get/toggle, EXTI;
- `ISpi` — mode 0..3, baud, bit order, full-duplex, polling/IRQ/DMA;
- `IPwm` — freq/duty/start/stop; caps для LEDC/таймеров;
- `IUart`, `II2c`, `IAdc`;
- `IFlash`, `IWatchdog`, `IPower`, `INvs`;
- `INetwork`, `IBle`.

## Правила

- все методы `noexcept`, возвращают `Status`;
- никаких `std::function`, `std::string`, исключений, RTTI;
- объекты статические; интерфейс передаётся по ссылке, не владеет;
- виртуальные вызовы не используются из ISR; для ISR — колбэки/очереди;
- `dynamic_cast` недоступен (`-fno-rtti`); возможности — через `caps()`/enum;
- `NotSupported` — легитимный ответ.

## Цена

- vptr в каждом объекте (4 байта) + vtable во Flash;
- на F103 20 КБ RAM: per-pin `IGpioPin` может быть дорогим, поэтому
  допускается `IGpioPort` с номером пина;
- для SPI/сети накладные расходы виртуального вызова ничтожны по сравнению
  с транзакцией; для GPIO-битбанга — измерить.

## SPI

`ISpi` — шина; `SpiDevice` — обёртка с CS-пином и блокировкой шины. Драйверы
устройств (Flash/SD/радио/датчики/дисплеи) зависят от `SpiDevice&`, а не от
STM32. Это и есть «SPI как интерфейс для чего угодно».

## Последствия

- Пример перестаёт включать `uart_stm32.hpp`; появляется `board::`.
- Нужны mock-реализации всех интерфейсов для host-тестов.
- Phase 1 вводит интерфейсы; Phase 3/4 — реализации.
