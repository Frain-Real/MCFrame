# MCFrame

Декларативный фреймворк для микроконтроллеров (ARM Cortex-M3, STM32F103).

## Сборка

```bash
./build.sh              # mock: сборка и запуск логики ОС на ПК
./build.sh stm32        # прошивка для STM32F103
./build.sh stm32 Release
```

Либо напрямую через CMake:

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mock.cmake
cmake --build build --parallel
./build/example/example_host
```

Платформа выбирается **toolchain-файлом**, а не правкой компилятора внутри
`CMakeLists.txt`: это единственный способ, при котором `-mcpu/-mthumb`
не теряются при любых внешних `-DCMAKE_*_FLAGS`.

## Архитектура

```
src/os/
  iplatform.hpp          контракт ядра с платформой (embed::platform::Ops)
  common.hpp             Status, Lock, общие типы
  config/                конфигурация таймеров и DMA
  core/primitives/       списки, очереди, кольцевой буфер, mutex, semaphore
  core/task/             задача и планировщик
  core/memory/           менеджер памяти, аллокатор
  time/                  модуль времени, таймеры, будильники
  interfaces/            CRTP-интерфейсы (UART, I2C, DMA)
  adc/                   драйвер АЦП
src/platforms/
  mock/                  реализация для ПК (тесты и отладка)
  stm32/                 реализация для STM32F103
```

Ядро **не содержит** адресов периферии и магических чисел: всё, что
зависит от платформы, приходит через `embed::platform::ops()`.

## Как подключить своё приложение

Приложение видит только заголовки ядра (`os/...`); платформенные заголовки
объявлены `PRIVATE` и недоступны по построению.

```cpp
// my_app.cpp
#include "os/core/memory/memory_manager.hpp"
#include "os/core/task/sheduler.hpp"
#include "os/config/time_config.hpp"

static embed::task     my_tcb;
static uint32_t        my_stack[192];

static void my_task(void*) noexcept {
    for (;;) {
        // ... полезная работа ...
        (void)embed::scheduler::instance().sleep_for(100);
    }
}

int main() {
    // 1. Память: адреса регионов даёт платформа, размеры — приложение.
    if (embed::g_memory_manager.init(4096, 512) != embed::Status::Ok) return 1;

    // 2. Время.
    embed::config::OsTimer::Config cfg{};
    if (embed::config::OsTimer::Init(cfg) != embed::Status::Ok) return 1;

    // 3. Планировщик (singleton: хук можно регистрировать без контекста).
    auto& sched = embed::scheduler::instance();
    if (sched.init() != embed::Status::Ok) return 1;
    if (embed::config::OsTimer::RegisterSystemHook(&embed::scheduler::tick_hook, nullptr)
        != embed::Status::Ok) return 1;
    if (embed::config::OsTimer::Start() != embed::Status::Ok) return 1;

    // 4. Задача: one-call регистрация (стек + ready).
    if (my_tcb.init(&my_task, nullptr, "my", 192, 2) != embed::Status::Ok) return 1;
    if (!sched.add_task(&my_tcb, my_stack, 192)) return 1;

    sched.start();      // на МКУ не возвращается
    return 0;
}
```

```cmake
# CMakeLists.txt приложения
cmake_minimum_required(VERSION 3.20)
set(MCU_TARGET stm32 CACHE STRING "")
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/MCFrame/cmake/toolchain-${MCU_TARGET}.cmake)
project(my_app C CXX ASM)
add_subdirectory(MCFrame)
add_executable(my_app my_app.cpp)
target_link_libraries(my_app PRIVATE MCFrame::MCFrame)
```

Либо после `make install` фреймворка — `find_package(MCFrame)` +
`MCFrame::MCFrame`.

## Что нужно знать при интеграции

| Тема | Правило |
|---|---|
| Стек задачи | Передавайте **статический** массив: `add_task(t, stack, words)` |
| Сон | `scheduler::sleep_for(ticks)` — сам инициирует переключение |
| Критическая секция | `embed::Lock` (сохраняет/восстанавливает PRIMASK) |
| Ожидание события | `semaphore::wait()` / `mutex::lock()` — блокируют задачу, а не крутят CPU |
| Из прерывания | `semaphore::signal_from_isr()`, `task::*_from_isr`-пути, без `Lock` |
| Логи | `embed::platform_log_line("...")` — на STM32 уходит в ITM/SWO |
| Регионы памяти | Меняются в `src/platforms/stm32/linker.ld` (`_OS_Heap_Size`, `_OS_StackPool_Size`) |

## Асинхронный ввод-вывод с линейным кодом

`os/async/` позволяет писать последовательный код, а внутри работает
асинхронный механизм — **без пользовательских колбэков**:

```cpp
#include "os/async/async_port.hpp"

async::AsyncPort<MyUart> port;

port.writeLine("AT");                    // выглядит блокирующим
// следующая строка — только после передачи
uint8_t buf[32]; Size n;
port.read(buf, sizeof buf, n, 100u);     // ждёт данные, не крутя CPU
port.readUntil(buf, sizeof buf, n, '\n', 500u);
```

Внутри задача **усыпляется**, а будит её обработчик завершения
(прерывание или DMA). Стратегия (Polling / Async / Dma) задаётся
интерфейсом, а не прикладным кодом:

| Стратегия | Кто завершает операцию | Когда выбирать |
|---|---|---|
| `Polling` | сам HAL из `poll_on_irq()`, вызываемого в цикле ожидания | нет прерываний/DMA, простой код |
| `Async` | `onRxComplete()` / `onTxComplete()` из обработчика | есть прерывания UART/SPI |
| `Dma` | `onDmaComplete()` из обработчика DMA | большие объёмы, минимум нагрузки на CPU |

Как реализовать HAL — см. `tests/test_async.cpp` (три готовые
trait-структуры) и `example/user_app.cpp` (`LogUart`).

Объект завершения (`async::completion`) доступен и отдельно, если нужен
свой протокол: `reset()` → `begin()` → `wait()`, а завершение — через
`post_ok()` / `post_error()` из прерывания. `post()` идемпотентен:
повторное завершение (типичный случай HT+TC у DMA) отбрасывается.

## UART и I2C

Оба интерфейса работают по одному принципу: **приложение пишет линейный код,
стратегия выбирается драйвером**. Доступны три стратегии — Polling, Async
(прерывания) и Dma.

```cpp
#include "os/async/async_hal.hpp"
#ifdef MCFRAME_TARGET_MCU
  #include "uart_stm32.hpp"      // USART1
  #include "i2c_stm32.hpp"       // I2C1
#else
  #include "uart_mock.hpp"       // эмуляция для ПК
  #include "i2c_mock.hpp"
#endif

using UartDriver = embed::stm32::UartDriver1;
embed::async::UartPort<UartDriver> uart;

UartDriver::Config cfg{};
cfg.baudRate = 115200u;
UartDriver::instance().configure(cfg);
UartDriver::instance().open();

uart.writeLine("AT");                        // линейно
uart.write("payload", 7u);
uint8_t buf[32]; Size n;
uart.readExact(buf, 4u, 100u);               // ровно 4 байта или Timeout
uart.readLine(buf, sizeof buf, n);           // до '\n', CR отбрасывается

// I2C: адрес — часть операции
embed::async::I2cPort<embed::stm32::I2cDriver1> i2c;
uint8_t t = 0u;
i2c.readRegister(embed::I2cAddress{0x48u}, 0x00u, t, 50u);
i2c.writeRegister(embed::I2cAddress{0x48u}, 0x01u, 0x60u, 50u);

bool found[128];
i2c.scan(found);                             // поиск устройств на шине
```

### Что делает каждая стратегия

| Стратегия | Кто завершает операцию | Где реализовано |
|---|---|---|
| `Polling` | драйвер в `pollOnIrq()`, который порт вызывает в цикле ожидания | `UartMock<Polling>`, `I2cDriver1` |
| `Async` | `USART1_IRQHandler` → `onRxIrq()/onTxIrq()`, `I2C1_EV/ER_IRQHandler` | `UartMock<Async>`, `I2cDriver1` (события) |
| `Dma` | обработчик DMA → `onDmaComplete()` | `UartDriver1`, `UartMock<Dma>` |

Приложение от этого не зависит: `AsyncPort::write()`/`read()` работают
одинаково. Это проверено тестами — один и тот же набор проверок
прогоняется по всем трём стратегиям.

### Как написать свой драйвер

Драйвер наследует `IUart<СвойДрайвер>` или `II2c<СвойДрайвер>` и реализует:

```cpp
static constexpr async::Strategy kStrategy;   // Polling/Async/Dma
static constexpr bool           kHasDma;
static constexpr Size           kMaxChunk;    // предел одного обмена
static СвойДрайвер& instance() noexcept;      // единственный экземпляр

Status configure(const Config&) noexcept;
Status open()/close()/ioctl(...) noexcept;

Status setCompletion(async::completion&) noexcept;   // владелец — порт
Status startWrite(...)/startRead(...) noexcept;      // вернуть управление сразу
Status abort() noexcept;
void   pollOnIrq() noexcept;                         // опрос (для Polling)
```

Ключевое правило: **`startWrite`/`startRead` не ждут завершения**. Они
запускают передачу и возвращают `Status::Ok`, а завершение приходит позже
через `completion`, который привязан вызовом `setCompletion()`.

## Потребление RAM и его настройка

Ядро настраивается под конкретный МКУ, а не «с запасом»:

| Что влияет | Где настраивается | По умолчанию |
|---|---|---|
| Число ядер в очередях готовых | `-DMCFRAME_MAX_CORES=N` | 1 |
| Уровней приоритета | `-DMCFRAME_MAX_PRIORITIES=N` | 8 |
| Учитываемых задач | `-DMCFRAME_MAX_TASKS=N` | 8 |
| Таймеров / будильников | `-DMCFRAME_TIMER_COUNT` / `-DMCFRAME_ALARM_COUNT` | 4 / 2 |
| Куча ОС | `-DMCU_OS_HEAP_BYTES=N` | 512 (0 = выключена) |
| Пул стеков задач | `-DMCU_OS_STACK_POOL_WORDS=N` | 0 (выключен) |
| Стек MSP | `-DMCU_MSP_STACK_BYTES=N` | 1K |

`0` для кучи/пула означает «регион не резервируется»: линкер не выделяет
память, а ядро сообщает нулевую ёмкость (`heap_available() == false`).
Приложению со статическими TCB и стеками куча не нужна вообще.

Размеры стеков задач подбираются **измерением**, а не на глаз:

```cpp
g_memory_manager.reset_all_high_water();   // сбросить водяной знак
// ... дать задачам поработать под реальной нагрузкой ...
g_memory_manager.update_all_high_water();
Size free_words = g_memory_manager.task_stack_high_water(&my_tcb);
// free_words — сколько слов стека ОСТАЛОСЬ свободными на пике.
// Оставьте запас ~30 % и уменьшите массив.
```

Готовый отчёт печатает `user_app_report_ram()` в примере: статические
ёмкости ядра, кучу, пул и водяные знаки стеков.

Ориентир для STM32F103RB (20 КБ RAM): пример с двумя задачами, АЦП и
самотестами занимает **3.7 КБ (18 %)**.

## Требования

* CMake ≥ 3.20
* mock: GCC/Clang (проверено на GCC 16)
* stm32: `arm-none-eabi-gcc` + `arm-none-eabi-gcc-ar` (для LTO)

## Тесты

```bash
./build.sh mock
ctest --test-dir .build/mock --output-on-failure
# или напрямую, чтобы увидеть все 40 проверок:
.build/mock/tests/mc_tests
```

Набор тестов (`tests/test_os.cpp`) фиксирует каждый исправленный дефект:
арифметику `BigInt` (деление, переносы, алиасинг), бухгалтерию кучи
(статистика, двойной `free`, фрагментация), работу `add_task`/`remove_task`
и корректность утилит.

mock-сборка компилируется с `-Wall -Wextra -Wconversion -Wshadow` и
запускается под ASan/UBSan (`-DMCFRAME_SANITIZE=ON`, включено по умолчанию) —
это ловит порчу памяти и UB, которые на МКУ проявляются как HardFault.
Тесты выполняются только для `mock`: они используют платформенные заглушки.
