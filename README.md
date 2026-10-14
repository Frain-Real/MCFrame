# MCFrame

Декларативный фреймворк для микроконтроллеров (поддерживаемые архитектуры: ARM Cortex-M3, STM32F103).

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
  core/primitives/       atomic_t, списки, очереди, кольцевой буфер, mutex, semaphore
  core/task/             задача и планировщик
  core/memory/           менеджер памяти, аллокатор
  time/                  модуль времени, таймеры, будильники
  interfaces/            CRTP-интерфейсы (UART, I2C, DMA)
  adc/                   драйвер АЦП
  crypto/                криптография: CRC, SHA-2/3, HMAC, AES, 3DES, RSA, ECDH
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
| Пул стеков | `stack_alloc()` / `stack_free()` — строго **LIFO**: вернуть можно только регион с вершины пула, иначе `Status::NotSupported` |
| Завершение задачи | `scheduler::exit()` из самой задачи или `remove_task(t)` снаружи: в обоих случаях ядро сразу получает новую задачу |
| Сон | `scheduler::sleep_for(ticks)` — сам инициирует переключение |
| Критическая секция | `embed::Lock` (сохраняет/восстанавливает PRIMASK) |
| Ожидание события | `semaphore::wait()` / `mutex::lock()` — блокируют задачу, а не крутят CPU |
| Из прерывания | `semaphore::signal_from_isr()`, `task::*_from_isr`-пути, без `Lock` |
| Атомарный доступ | `embed::atomic_t<T>`: `load`/`store`/`exchange`/`compare_exchange_strong`/`fetch_add` — не `std::atomic` и не «голые» `__atomic_*` |
| Логи | `embed::platform_log_line("...")` — на STM32 уходит в ITM/SWO |
| Регионы памяти | Меняются в `src/platforms/stm32/linker.ld` (`_OS_Heap_Size`, `_OS_StackPool_Size`) |

## Атомарные переменные

Единственный способ атомарного доступа в ядре — `embed::atomic_t<T>`
(`os/core/primitives/types.hpp`):

```cpp
#include "os/core/primitives/types.hpp"

embed::atomic_t<uint32_t> g_events{0u};      // счётчик
embed::atomic_bool        g_done{false};     // защёлка

(void)g_events.fetch_add(1u, embed::MemoryOrderRelaxed);   // прежнее значение
if (g_done.compare_exchange_strong(expected, true)) { /* выиграли защёлку */ }
```

| Вместо | Используйте | Почему |
|---|---|---|
| `std::atomic<T>` | `embed::atomic_t<T>` | `<atomic>` тянет libstdc++; для 64-битных типов на Cortex-M3 нужен libatomic, которого нет в линковке прошивки |
| `__atomic_load_n(&x, ...)` | `x.load(...)` | встроенная функция не проверяет тип и одинаково компилируется для любого поля |
| `volatile` как синхронизация | `atomic_t` | `volatile` не даёт ни атомарности, ни порядка доступа к памяти |

Порядок по умолчанию — `MemoryOrderSeqCst`; ослабляйте его только там, где
доступ уже взаимоисключён (`Lock`) или гонки нет по построению, и поясняйте
это комментарием на месте. Типы шире машинного слова запрещены
(`static_assert`) — для 64-битных счётчиков берите `Lock`.

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

## Криптография

Модуль `os/crypto/` — статические алгоритмы без динамической памяти, без
исключений и без виртуальных функций. Точка входа одна:

```cpp
#include "os/crypto/crypto.hpp"
```

| Файл | Что даёт |
|---|---|
| `crypto_utils.hpp` | порядок байт (big/little endian), сравнение за постоянное время, PKCS#7 |
| `random.hpp` | CSPRNG на ChaCha20 (RFC 8439) — нужен для ключей |
| `crc.hpp` | CRC-8/16/32: обычные и «отражённые» (zlib, MODBUS), плюс аппаратный CRC-32 у STM32 |
| `sha2.hpp` | SHA-224/256/384/512, SHA-512/224, SHA-512/256 |
| `sha3.hpp` | SHA3-224/256/384/512, SHAKE128/256 (XOF) |
| `hmac.hpp` | HMAC-SHA256 (потоковый) |
| `aes.hpp` | AES-128/192/256: ECB, CBC, CTR (+PKCS#7) |
| `tdes.hpp` | DES и Triple-DES (EDE): ECB, CBC, CTR |
| `rsa.hpp` | RSA на 16/32-битных целых — **только для отладки и учебных задач** |
| `ecc.hpp` | ECDH: secp256k1, secp256r1 (P-256) |
| `ecies.hpp` | гибридное шифрование: ECDH + KDF на SHA-256 + HMAC |

Единые правила модуля (то, чего не было в первой версии):

* **Ёмкость выходного буфера передаётся явно**, результат — `embed::Status`.
  Признаком ошибки больше не служит возврат `0`.
* **Секреты затираются** (`wipe`/`clear`), ключевой материал не остаётся ни
  в буферах, ни на стеке.
* **Проверяются входные данные**: ключи, IV, длины, точки на кривой.
* **Порядок байт фиксирован стандартом** — результат одинаков на ПК и на МКУ.
* **Ничего не «само» не безопасно**: генерация ключей принимает только CSPRNG
  (`crypto::Rng`), засеянный энтропией платформы; CBC/CTR без MAC не дают
  целостности — для этого есть `hmac_sha256` и `Ecies`.

```cpp
using namespace embed::crypto;

uint8_t digest[Sha256::DigestSize];
Sha256::hash("abc", 3u, digest);                 // 187 проверок в tests/test_crypto.cpp

uint8_t key[16] = { /* ... */ };
uint8_t iv[16]  = { /* ... */ };
Aes128CbcPkcs7 aes(key, iv);
uint8_t out[64]; Size out_len = 0u;
if (aes.encrypt(plain, plain_len, out, sizeof(out), &out_len) != Status::Ok) { /* ... */ }

// Обмен ключами
using D = Ecdh<EccCurve::Secp256r1>;
D::Scalar priv; D::Point pub;
D::generate_key_pair(rng, priv, pub);            // rng — crypto::Rng с энтропией
uint8_t secret[32];
D::derive_shared_secret_bytes(priv, peer_pub, secret);
```

Занимаемый объём (замер `arm-none-eabi-size` для Cortex-M3, `-O2`,
`-ffunction-sections -fdata-sections --gc-sections`): неиспользованный модуль
в прошивку **не попадает вообще**, а по отдельности алгоритмы стоят примерно
так — SHA-256 ≈ 0.8 КБ, AES-128-CBC+PKCS#7 ≈ 1.5 КБ, RSA (32-битный) ≈ 2 КБ,
3DES ≈ 2.6 КБ, ECDH на P-256 ≈ 4 КБ. Все алгоритмы сразу — ≈ 24 КБ FLASH.

### Ограничения, о которых нужно знать

* `rsa.hpp` работает с модулем меньше 2³² — такой ключ **не стоек**;
  модуль нужен для интерфейсов с уже скомпрометированной стойкостью и для
  отладки. Стойкий RSA требует `BigInt<64>` и отдельной реализации.
* ECC — программная реализация: скалярное умножение лестницей Монтгомери,
  арифметика поля — умножение Монтгомери (CIOS, без деления). На ПК (`-O2`)
  одно скалярное умножение занимает ≈0.5 мс, то есть на Cortex-M3 (72 МГц)
  это десятки миллисекунд: для обмена ключами годится, для потока — нет.
  Это также НЕ защита от атак по побочным каналам (время/потребление).
* `Cbc`/`Ctr` без HMAC не защищают от подмены: правка шифротекста не
  обнаруживается. Для «шифрование + целостность» используйте `Ecies` или
  вручную `Aes`+`hmac_sha256`.

## Требования

* CMake ≥ 3.20
* mock: GCC/Clang (проверено на GCC 16)
* stm32: `arm-none-eabi-gcc` + `arm-none-eabi-gcc-ar` (для LTO)

## Тесты

```bash
./build.sh mock
ctest --test-dir .build/mock --output-on-failure
# или напрямую:
.build/mock/tests/mc_tests          # ядро: 40 проверок
.build/mock/tests/mc_tests_crypto   # криптография: 187 проверок
```

`ctest` прогоняет четыре набора: `core_selftest`, `async_io`, `uart_i2c` и
`crypto`.

Набор тестов (`tests/test_os.cpp`) фиксирует каждый исправленный дефект ядра:
арифметику `BigInt` (деление, переносы, алиасинг), бухгалтерию кучи
(статистика, двойной `free`, фрагментация), работу `add_task`/`remove_task`
и корректность утилит.

Набор `tests/test_crypto.cpp` построен на **стандартных тестовых векторах**
(FIPS 180-4/197/202, NIST SP 800-38A, RFC 4231, SEC 2/FIPS 186-4, каталог CRC)
и на проверках каждого исправленного дефекта модуля: константы SHA-2, усечение
дайджеста, расшифрование AES-CTR без таблицы обратных подстановок, размер
блока TDES, переполнение модуля RSA, точки кривых ECC, обнаружение подмены в
ECIES.

mock-сборка компилируется с `-Wall -Wextra -Wconversion -Wshadow` и
запускается под ASan/UBSan (`-DMCFRAME_SANITIZE=ON`, включено по умолчанию) —
это ловит порчу памяти и UB, которые на МКУ проявляются как HardFault.
Тесты выполняются только для `mock`: они используют платформенные заглушки.
