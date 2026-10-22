# MCFrame
**Проект пишется мной и нейросетью. Проект находится в сыром виде (альфа версия).**

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

### LTO

Межмодульная оптимизация включается автоматически для оптимизированных сборок
(`Release`, `RelWithDebInfo`, `MinSizeRel`) и выключена в `Debug`: при `-O0`
она почти ничего не даёт, зато усложняет отладку и удлиняет сборку.

```bash
./build.sh stm32 Release          # LTO включён
./build.sh stm32                  # Debug: LTO выключен
./build.sh stm32 Release -DMCFRAME_LTO=OFF      # Release без LTO
./build.sh stm32 -DMCFRAME_LTO=ON               # Debug с LTO
```

Режим задаётся трёхзначно: `-DMCFRAME_LTO=AUTO|ON|OFF` (по умолчанию `AUTO`).
Именно `AUTO`, а не «bool, включённый для Release»: значение по умолчанию,
однажды попавшее в кэш, осталось бы там навсегда, и переконфигурирование уже
существующего каталога с Debug на Release молча оставляло бы LTO выключенным
(проверено — так и происходило). `AUTO` пересчитывается при каждом запуске
cmake и печатается в отчёте конфигурации.

Выигрыш в этом проекте заметнее обычного, потому что ядро — **статическая
библиотека**: без LTO границы модулей (планировщик ↔ энергосбережение ↔
сторож ↔ драйверы) для оптимизатора так же непрозрачны, как границы файлов, а
через них идёт горячий путь (тик, простой, проверка сроков). Замер на примере
(STM32F103RB, `-O2`, 128 КБ Flash / 20 КБ RAM, `arm-none-eabi-size` и отчёт
линкера `--print-memory-usage`):

| Сборка | `.text` | `.data` | `.bss` | FLASH | RAM |
|---|---|---|---|---|---|
| Release | 31204 | 2584 | 2672 | 33788 Б (25.8 %) | 5264 Б (25.7 %) |
| **Release + LTO** | **20188** | 2340 | 2168 | **22528 Б (17.2 %)** | **4504 Б (22.0 %)** |
| Release + `DEEP_SLEEP=ON` | 32248 | 2584 | 2688 | 34832 Б (26.6 %) | 5280 Б (25.8 %) |
| Release + LTO + `DEEP_SLEEP=ON` | 28524 | 2588 | 2680 | 31112 Б (23.7 %) | 5272 Б (25.7 %) |

Что важно понимать про эти цифры:

* **LTO не гарантирует уменьшения.** Без глубокого сна выигрыш 33 % Flash
  (33.8 → 22.5 КБ), а с ним — только 11 % (34.8 → 31.1 КБ): межмодульный
  инлайнер забрал себе часть экономии, влив в `main` функции, вызываемые
  один раз (в этом варианте `main` вырос с 636 до 5976 байт). LTO — не
  «кнопка уменьшения», а перераспределение: размер смотрят
  `arm-none-eabi-size` после сборки, а не верят флагу;
* **LTO не отменяет `-ffunction-sections -fdata-sections` + `--gc-sections`**:
  они выкидывают недостижимый код (например, неиспользуемую криптографию) и
  работают вместе с LTO;
* **нужен `gcc-ar`** (`arm-none-eabi-gcc-ar`): индекс символов LTO-объектов
  строит плагин, обычный `ar` даёт «plugin needed to handle lto object».
  В toolchain-файлах архиватор уже прописан, но проверить стоит: без него
  сборка библиотеки и линковка прошивки не сойдутся;
* **проверяйте не только размер, но и работоспособность**: LTO переставляет и
  инлайнит код, а в проекте есть `__attribute__((naked))` (сохранение и загрузка
  контекста задач) и таблица векторов в `startup_stm32f10x_ld.s`. После смены
  флага имеет смысл убедиться, что символы обработчиков на месте
  (`arm-none-eabi-nm firmware | grep -E "Reset_Handler|SysTick_Handler|main"`),
  а `.isr_vector` не пуст (`arm-none-eabi-objdump -h firmware`);
* **LTO видит ВЕСЬ граф вызовов и потому выкидывает код, до которого нет
  вызовов** — даже если адрес функции лежит в таблице `platform::Ops`. Это
  полезно (LTO сразу показывает «висячие» адреса), но обнажает и обратную
  проблему: **ссылки из встроенного asm компилятор не видит**. Два реальных
  примера из этого дерева, оба ловятся только под LTO:
  * символы, на которые ссылается ТОЛЬКО asm (обработчики переключения
    контекста и переменная `g_first_task_sp`, через которую `SVC_Handler`
    получает SP первой задачи), обязаны быть помечены
    `__attribute__((used, externally_visible))` — иначе LTO переименовывает их
    во внутренние, и линковка падает с `undefined reference to
    g_first_task_sp`;
  * `ldr r1, =symbol` внутри `naked`-функции требует литерального пула,
    которого там нет (naked — это чистый asm без `.pool`), и ассемблер падает
    с `offset out of range`. Адреса в обработчиках поэтому грузятся
    `movw`/`movt`;
* **тесты на LTO тоже прогоняются**: `./build.sh mock Release` собирает наборы
  с LTO (и с санитайзерами — эта комбинация также проверена).

Отдельные возможности включаются флагами сборки:

```bash
# Глубокий сон (STOP + будильник RTC) для STM32 — по умолчанию ВЫКЛЮЧЕН,
# см. «Энергосбережение: супервизор сна и статистика нагрузки»
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-stm32.cmake \
      -DMCU_TARGET=stm32 -DMCFRAME_STM32_DEEP_SLEEP=ON

# Отчёт об аварии (Fault/panic) включён по умолчанию. OFF экономит ~400 Б,
# если приложение не влезает в слот.
./build.sh stm32 Release -DMCFRAME_IMAGE=slot -DMCFRAME_FAULT_REPORT=OFF
```

## Архитектура

```
src/os/
  iplatform.hpp          контракт ядра с платформой (embed::platform::Ops)
  common.hpp             Status, Lock, общие типы
  config/                конфигурация таймеров и DMA
  core/primitives/       atomic_t, списки, очереди, кольцевой буфер, mutex, semaphore
  core/ipc/              MessageQueue, Mailbox, EventFlags (статические, ISR-safe)
  core/work/             WorkQueue: отложенная обработка прерываний
  core/task/             задача, планировщик, очередь ожидания, статистика
  power/                 супервизор энергосбережения: сон и статистика нагрузки
  watchdog/              сторожевой таймер: слежение за участниками и посмертная запись
  fault/                 диагностика аварий: Fault, crash record, panic/assert
  log/                   уровни, кольцевой буфер, запись из ISR
  shell/                 консоль: help/ps/free/stats/log/reboot + wd/ota
  ota/                   обновление прошивки: приём, проверка, активация слотов
  boot/                  загрузчик: выбор слота, проверка образа, переход
  core/memory/           менеджер памяти, аллокатор
  time/                  модуль времени, таймеры, будильники, DeferredTimer
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
| Энергосбережение | `power::Supervisor::instance().init(cfg)` + системный хук `Supervisor::tick_hook` (первым) — см. «Энергосбережение» |
| Сторожевой таймер | `watchdog::Service::init(cfg)` → `Participant::init(...)` у каждой задачи → хук `Service::tick_hook` → `Service::start()` последним — см. «Сторожевой таймер» |
| Смена контекста | Обработчики `PendSV_Handler`/`SVC_Handler` уже определены платформой; своих с этими именами не заводить — см. «Смена контекста: PendSV и SVC» |
| Критическая секция | `embed::Lock` (сохраняет/восстанавливает PRIMASK) |
| Ожидание события | `semaphore::wait()` / `mutex::lock()` — блокируют задачу, а не крутят CPU; таймаут абсолютный, очередь ожидания — по приоритету |
| Очередь сообщений | `MessageQueue<T,N>` — статическая, `push/pop` с таймаутом, `push_from_isr/pop_from_isr` |
| Почтовый ящик | `Mailbox<T>` — одно место, `post()` перезаписывает последнее значение |
| Группа событий | `EventFlags` — 32 бита, `wait(mask, all, clear_on_exit)` |
| Отложенная работа | `WorkQueue<N>` — ISR/таймер делает `post()`, задача исполняет; колбэки не работают в прерывании |
| Статистика задач | `task::runtime_ticks()/switches()`, `scheduler::task_snapshot()` |
| Round-robin | `scheduler::set_time_slice(ticks)` или `-DMCFRAME_TIME_SLICE_TICKS=N` (0 — выкл.) |
| Логи | `MCFRAME_LOG_INFO(tag, msg)`, `log::from_isr()`, `log::pop()` — кольцо для shell |
| Shell | `shell::Console` + `add_watchdog_commands()` / `add_ota_commands()` |
| Авария | `MCFRAME_ASSERT(cond)`, `MCFRAME_PANIC(kind)`, `fault::last()/report()`; запись переживает reset — см. «Диагностика отказов» |
| Переполнение стека | Маркер дна проверяется в переключении контекста; приложение может спросить `scheduler::stack_guard_ok(t)` |
| Из прерывания | `semaphore::signal_from_isr()`, `task::*_from_isr`-пути, без `Lock` |
| Атомарный доступ | `embed::atomic_t<T>`: `load`/`store`/`exchange`/`compare_exchange_strong`/`fetch_add` — не `std::atomic` и не «голые» `__atomic_*` |
| Логи | `embed::platform_log_line("...")` — на STM32 уходит в ITM/SWO |
| Регионы памяти | Меняются в `src/platforms/stm32/linker.ld` (`_OS_Heap_Size`, `_OS_StackPool_Size`) |
| Загрузчик | Образ `bootloader/bootloader_fw` с 0x08000000, приложение — в слот (`-DMCFRAME_IMAGE=slot`). `boot::Loader` решает, `platform::BootOps::jump` переходит; приложение подтверждает загрузку через `boot::confirm_self()` — см. «Загрузчик» |
| OTA | `ota::Service::init(cfg)` — ГДЕ хранить и ПО ЧЕМУ принимать; постоянный `poll()` в задаче; `boot_check()`/`confirm_running()` при загрузке. Носитель и интерфейс обязаны жить дольше сервиса — см. «OTA: приём новой прошивки» |

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

## Ожидание объектов: очередь, таймаут, приоритет

Семафор и мьютекс больше не полагаются на «первую задачу в реестре». У
каждого объекта своя очередь ожидания (`embed::wait_queue`):

* порядок — по приоритету (0 — высший), внутри одного приоритета FIFO;
* `signal()`/`unlock()` будят **самую приоритетную** задачу, а не случайную;
* задача с таймаутом стоит одновременно в очереди объекта и в `sleep_list_`,
  поэтому её можно разбудить и событием, и сроком — без потери учёта.

### Одна критическая секция на проверку и сон

Прежний `wait()` проверял счётчик под `Lock`, отпускал её и только потом
вызывал `block_on()`. В этом окне прерывание могло сигналить: счётчик
становился 1, `unblock_one()` не находил ещё не заблокированную задачу, и она
засыпала навсегда. Теперь проверка предиката и постановка в очередь — одна
операция:

```cpp
Status wait(uint32_t ticks = 0) noexcept {
    const uint64_t deadline = ticks ? (now_ticks() + ticks) : 0u;
    for (;;) {
        Lock l;                         // прерывания запрещены
        if (count_locked() > 0u) { take_locked(); return Status::Ok; }
        // block_on_locked ставит в очередь, снимает Lock и уступает ядро
        const Status st = block_on_locked(l, waiters_, this, left(deadline));
        if (st != Status::Ok) return st;
    }
}
```

### Абсолютный таймаут

`deadline` считается **один раз** до цикла. Серия ложных пробуждений
(«разбудили, но ресурс забрал другой») больше не продлевает ожидание: каждый
следующий `block_on_locked()` получает остаток до того же срока и возвращает
`Status::Timeout`, когда срок истёк. Прежний код на каждой итерации снова
передавал полный `ticks`, поэтому `wait(100)` под конкуренцией мог ждать
бесконечно.

### Priority inheritance и ceiling

`mutex` умеет и то, и другое:

```cpp
embed::mutex m;                       // обычный
embed::mutex r(true);                 // рекурсивный
embed::mutex c(false, 2u);            // ceiling: владелец поднимется до 2

// PI включён всегда: пока high ждёт, low-владелец наследует его приоритет.
// На unlock приоритет восстанавливается к назначенному (base_priority).
```

`Task::priority()` — действующий приоритет (его видят очереди),
`base_priority()` — назначенный приложением. `scheduler::set_priority()`
меняет оба, `set_effective_priority()` — только действующий (так работает
PI/ceiling).

### Ограничения

* Реализовано **одноуровневое** наследование. Вложенные мьютексы с разными
  потолками могут временно оставить приоритет выше базового; для F103 с 8
  приоритетами это осознанный компромисс.
* Задача, завершившаяся с захваченным мьютексом, оставляет его занятым:
  ядро не ведёт реестр «кто какие мьютексы держит». Не вызывайте `exit()`
  внутри критической секции; для долгоживущих систем добавьте внешний
  watchdog-контроль владельца.
* `try_lock()` не блокируется и не поднимает приоритет владельца.

## Очереди сообщений, почтовый ящик, события

Три примитива над `wait_queue`, без динамической памяти и с ISR-вариантами.

```cpp
#include "os/core/ipc/message_queue.hpp"
#include "os/core/ipc/mailbox.hpp"
#include "os/core/ipc/event_flags.hpp"

embed::MessageQueue<MyMsg, 8> q;       // 8 сообщений, память в объекте
MyMsg m{};
q.push(m);                              // без таймаута
q.push(m, 100);                         // ждать до 100 тиков -> Status::Timeout
q.push_from_isr(m);                     // из прерывания, не блокируется
q.pop(m, 50);

embed::Mailbox<uint32_t> latest;        // одно место, последнее важнее
latest.post(42);                        // перезаписывает, не блокируется
latest.pend(v, 100);                    // ждёт появления
latest.post_from_isr(43);

embed::EventFlags events;
events.set(0x01);                       // из задачи
events.set_from_isr(0x02);              // из прерывания
events.wait(0x03, /*all*/ true, /*clear*/ true);
```

Правила те же, что у семафора/мьютекса:

* проверка места/данных и постановка в очередь — **одна критическая секция**,
  поэтому сигнал из ISR не теряется;
* очередь ожидания — по приоритету, внутри приоритета FIFO;
* таймаут абсолютный;
* `pop` будит ожидающего писателя, `push` — ожидающего читателя;
* `MessageQueue<T,N>` хранит СЫРОЕ выравненное хранилище и создаёт элементы
  placement new: `T` не обязан быть default-constructible. Требуются
  copy-constructible/copy-assignable `T`; деструктор вызывается при `pop` и в
  деструкторе очереди.

`EventFlags::set()` будит **всех** ожидающих: каждый перепроверяет свою маску
под Lock и либо забирает событие, либо снова засыпает. Это дороже «разбудить
одного», но единственно корректно для «любое из»/«все из».

## Отложенная обработка: WorkQueue

Колбэки таймеров (`TimerModule`) и обработчики прерываний выполняются в
контексте исключения: в них нельзя ждать, брать мьютекс или делать долгую
работу. `WorkQueue` переносит работу в обычную задачу:

```cpp
#include "os/core/work/work_queue.hpp"

embed::WorkQueue<8> jobs;
static uint32_t jobs_stack[128];

void handle_sample(void*) noexcept;

void on_timer(void*) noexcept {           // контекст ISR
    jobs.post_from_isr(&handle_sample, nullptr);
}

int main() {
    (void)jobs.start("jobs", jobs_stack, 128, /*priority*/ 5);
    // ...
}

void handle_sample(void*) noexcept {      // обычный контекст задачи
    // здесь можно ждать, брать мьютекс, писать во Flash
}
```

`post()` можно вызывать до `start()` — работа не потеряется, задача заберёт её
при первом запуске. Если очередь заполнена, `post()` возвращает `false`, а не
блокируется. Стек задачи передаёт приложение: WorkQueue не выделяет память.

### Таймеры без работы в ISR: `DeferredTimer`

`TimerModule` по-прежнему вызывает колбэки из `TickIsr`. Чтобы не писать
блокирующий код в прерывании, используйте обёртку:

```cpp
#include "os/time/deferred_timer.hpp"

embed::DeferredTimer<8> dt;
static uint32_t jobs_stack[128];
embed::WorkQueue<8> jobs;

void on_sample(void*) noexcept {          // обычный контекст задачи
    // можно ждать, брать мьютекс, писать во Flash
}

jobs.start("jobs", jobs_stack, 128, 5);
dt.create(jobs, /*period_ms*/ 100, /*periodic*/ true, &on_sample);
dt.start();
```

ISR-колбэк `DeferredTimer` только делает `post_from_isr()`; если очередь
переполнена, событие теряется, но счётчик `dt.dropped()` это показывает.
`DeferredTimer` и `WorkQueue` должны жить дольше таймера.

## Round-robin

По умолчанию планировщик строго приоритетный: задача одного приоритета
владеет CPU, пока не заблокируется. Если в приложении есть равноправные
задачи, включите квант:

```cpp
embed::scheduler::instance().set_time_slice(10);   // 10 тиков
// или на этапе сборки: -DMCFRAME_TIME_SLICE_TICKS=10
```

Раз в квант текущая задача уходит в хвост очереди **своего** приоритета, и
ядро выбирает следующую. Задачи более высокого приоритета вытесняют её как
обычно; на разные приоритеты квант не влияет. `0` (по умолчанию) выключает
round-robin полностью, сохраняя детерминизм.

## Смена контекста: PendSV и SVC

Задачи на МКУ переключаются двумя обработчиками платформы
(`src/platforms/stm32/platform_ops.cpp`); в таблице векторов они занимают
`PendSV_Handler` и `SVC_Handler`, поэтому своих определений с этими именами в
приложении быть не должно.

| Обработчик | Когда работает | Что делает |
|---|---|---|
| `SVC_Handler` | один раз, при `scheduler::start()` | загружает контекст ПЕРВОЙ задачи и выходит из исключения в неё |
| `PendSV_Handler` | при каждом `yield()`, `sleep_for()`, `exit()` и вытеснении по тику | сохраняет контекст уходящей задачи, выбирает следующую, загружает её |

### Почему первое переключение — отдельный обработчик

У первого переключения нет уходящего контекста: ядро в этот момент работает в
потоковом режиме на MSP, а PSP ещё не инициализирован (задачи живут на PSP).
Обработчик переключения начинается с сохранения `R4-R11` на стек текущей
задачи, то есть по PSP — и записал бы регистры по мусорному адресу. Поэтому
`platform::Ops::start_first_task()` делает это иначе: запоминает SP первой
задачи, обнуляет MSP (стек `main` больше не нужен), разрешает прерывания и
входит в `SVC`, а `SVC_Handler` загружает контекст и выходит из исключения с
`EXC_RETURN = «потоковый режим, PSP»`. Классическая схема портов Cortex-M.

Прежний вариант дёргал `CONTROL.SPSEL` прямо в потоковом режиме и заканчивал
`bx 0xFFFFFFFD`: в потоковом режиме это не возврат из исключения, а переход по
адресу `0xFFFFFFFC`, то есть UsageFault — запуск первой задачи не работал.

### Почему обработчик PendSV сбалансирован по MSP

Переключение — три шага: сохранить `R4-R11` уходящей задачи, выбрать
следующую, загрузить её `R4-R11` и вернуться из исключения. Если бы загрузку
делал C-код (как `platform_load_context()` с `bx 0xFFFFFFFD`), кадры стека
обработчика остались бы в MSP навсегда: исключение уже не вернётся, а MSP никто
не размотает — за десятки переключений 1 КБ MSP кончились бы, и ядро затёрло бы
`.bss`. Поэтому:

```asm
PendSV_Handler:
    bl   platform_save_context     ; R4-R11 уходящей задачи → её PSP, r0 = SP
    cpsid i                        ; критическая секция: очередь готовых
    push {r4, lr}                  ; + выравнивание MSP по 8 (AAPCS)
    bl   embed_pendsv_switch       ; середина: выбор задачи, r0 = SP следующей
    pop  {r4, lr}                  ; MSP — как на входе в обработчик
    ldmia r0!, {r4-r11}
    msr  psp, r0
    isb
    cpsie i                        ; PRIMASK возвращаем в 0 (иначе задача встанет без прерываний)
    bx   lr                        ; EXC_RETURN: аппаратный кадр снимается со стека НОВОЙ задачи
```

Середина (`scheduler::prepare_context_switch()`) — обычная функция, которая
ВОЗВРАЩАЕТ SP следующей задачи; она же единственное место, где меняется
состояние задач, поэтому прерывания на это время запрещены (с прерываниями
периферии с более высоким приоритетом обработчик делил бы очередь готовых).
`platform_load_context()` из таблицы контракта никуда не делась, но
переключение ею не пользуется — ровно из-за утечки MSP.

### Тик только просит переключение

`scheduler::tick()` больше не вызывает `switch_to(next)`: прежде вытеснение по
тику переносило `current` на новую задачу ДО сохранения контекста уходящей, и
обработчик сохранял бы контекст старой задачи в TCB новой (портя её SP). Теперь
тик лишь выставляет PendSV (`check_preempt`), а всю работу делает обработчик —
как и при пробуждении задач из `sleep_list`.

### Что здесь проверяется на хосте

Реального переключения на ПК нет (mock-платформа не меняет контекст), поэтому
проверяется то, что можно: учётная середина — `scheduler::prepare_context_switch()`
(набор `mc_tests`: возвращается SP именно той задачи, которая стала текущей;
SP уходящей записан в её TCB; задача не потеряна). Сам asm проверяется
косвенно: сборкой под LTO и без неё, наличием обработчиков в таблице векторов
(`arm-none-eabi-nm`/`objdump -h`) и отсутствием в образе слабых заглушек
`Default_Handler` на месте `PendSV`/`SVC`.

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
cfg.baudRate   = 115200u;
cfg.strategy   = embed::async::Strategy::Async;   // Polling | Async | Dma
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

### Как выбирается стратегия

Одна и та же для mock и STM32:

```cpp
UartDriver::Config cfg{};
cfg.baudRate = 115200u;
cfg.strategy = embed::async::Strategy::Dma;   // Polling | Async | Dma
UartDriver::instance().configure(cfg);        // NotSupported, если режим не поддержан

UartDriver::instance().usePolling();          // сменить на лету
UartDriver::instance().useInterrupts();       // (требуется простой: на середине
UartDriver::instance().useDma();              //  операции вернётся Busy)
UartDriver::strategy();                       // что выбрано сейчас
```

По умолчанию у драйверов STM32 включены прерывания (`Async`): DMA требует
свободного канала, поэтому включается явно. У mock стратегия задана шаблоном
(`UartMock<Polling>` и т.п.), и `Config::strategy` только проверяется —
запрос другого режима даёт честный `NotSupported`.

### Что делает каждая стратегия

| Стратегия | USART1 (STM32) | I2C1 (STM32) |
|---|---|---|
| `Polling` | `pollOnIrq()` читает SR; `RXNEIE/TXEIE/TCIE/IDLEIE` и NVIC выключены, DMA не участвует | `pollOnIrq()` крутит автомат `pump()`; `CR2` IE и NVIC выключены |
| `Async` | прерывания `USART1_IRQHandler`: RXNE (байты), TXE (отдать байт), TC (передача завершена), IDLE (конец пачки) | `I2C1_EV_IRQHandler` (SB/ADDR/BTF/RXNE/TXE), `I2C1_ER_IRQHandler` (AF/BERR/ARLO) |
| `Dma` | DMA1 Ch4 (TX) / Ch5 (RX): `DMAT/DMAR`, завершение TX — по TC из `USART1_IRQHandler`, RX — по IDLE или заполнению буфера | DMA1 Ch6 (TX) / Ch7 (RX): `DMAEN`, события по-прежнему по прерываниям; приём — N-1 байт через DMA, последний байт программно (NACK+STOP до его приёма) |

Приложение от этого не зависит: `AsyncPort::write()`/`read()` работают
одинаково. Что результат совпадает во всех трёх режимах, проверяют два
набора: `uart_i2c` — на mock-драйверах, `stm32_hal_strategies` — на
настоящих STM32-драйверах, собранных на ПК против программной модели
регистров (см. «Тесты»).

### Арбитр каналов DMA: очередь вместо Busy

На STM32F1 запрос DMA **жёстко привязан к каналу** (remap запросов не
существует), поэтому интерфейсы делят каналы: `USART1_TX` и `I2C2_TX` — это
один и тот же DMA1 Channel4, `USART1_RX` и `I2C2_RX` — Channel5. Без арбитра
второй интерфейс просто получал `Status::Busy`, и его запрос терялся.

Арбитр (`os/interfaces/dma_arbiter.hpp`) превращает отказ в ожидание: работа
встаёт в очередь и выполняется, когда канал освободится.

```cpp
embed::async::UartPort<embed::stm32::UartDriver1> uart;
// Линейно: как обычный write, но при занятом канале работа ждёт в очереди.
uart.writeQueued("payload", 7u, /*timeout_ms*/ 100u);

embed::async::I2cPort<embed::stm32::I2cDriver1> i2c;
uint8_t t = 0u;
i2c.readRegisterQueued(embed::I2cAddress{0x48u}, 0x00u, t, 100u);

// Асинхронно: submit() + ready()/status()/transferred(), затем release().
embed::DmaJob job{};
job.tx    = UartDriver1::kTxRequest;
job.owner = &UartDriver1::instance();
job.start = &my_start;          // Status (*)(void* ctx, async::completion&)
job.ctx   = &my_ctx;
embed::DmaTicket ticket;
embed::DmaArbiter::instance().submit(job, ticket);
```

Что гарантируется:

| Свойство | Как достигается |
|---|---|
| канал занят одной работой | захват через `DmaManager`, поэтому «проигравший» не выставит `DMAT/DMAR/DMAEN` |
| разные каналы — параллельно | очередь не глобальная, а по каналам: поток АЦП не ждёт UART |
| один интерфейс — одна работа | у драйвера один объект завершения; работы с общим `owner` выстраиваются |
| нет взаимной блокировки | все каналы работы (например `I2C1_TX` + `I2C1_RX` у `writeRead`) захватываются **разом** |
| порядок FIFO | кто раньше попросил — тот раньше и поедет |
| таймаут считается от приёма | включая ожидание в очереди; истёк — работа снимается, слот и канал освобождаются |

Ограничения, о которых нужно знать:

* очередь **не делит** канал между перифериями внутри одной операции — это
  невозможно аппаратно; работы на общем канале последовательны;
* глубина очереди статическая (`config::DMA_ARBITER_JOBS`, по умолчанию 4 —
  это ~520 байт RAM); переполнение даёт честный `Status::NoMemory`;
* работа, которой интерфейс занят обменом **вне** арбитра (например вызовом
  `port.write()` напрямую), завершится статусом драйвера (обычно `Busy`);
* арбитр защищает только тех, кто ходит к DMA через `DmaManager` (АЦП в
  проекте настраивает канал напрямую — его канал арбитру не виден);
* обёртки `*Queued` требуют `Strategy::Dma`: иначе вернётся `NotSupported`
  (занимать канал под обмен по прерываниям бессмысленно).

### Как написать свой драйвер

Драйвер наследует `IUart<СвойДрайвер>` или `II2c<СвойДрайвер>` и реализует:

```cpp
// Стратегия: ЛИБО функция (режим можно менять на лету), ЛИБО константа.
static async::Strategy strategy() noexcept;   // как у STM32-драйверов
static constexpr async::Strategy kStrategy;   // как у mock: режим фиксирован

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
| Системных хуков | `-DMCFRAME_HOOK_COUNT=N` | 3 (ядро занимает 3: планировщик, энергосбережение, сторож) |
| Участников сторожа | `-DMCFRAME_WATCHDOG_SLOTS=N` | 6 |
| Источников ограничения сна | `power::Supervisor::kMaxSleepLimits` | 2 |
| Буферы кадров OTA | `-DMCFRAME_OTA_MAX_PAYLOAD=N` | 128 байт (буфер 141 Б) |
| Предел подписи OTA | `-DMCFRAME_OTA_SIGNATURE_MAX=N` | 80 байт |
| Запись об аварии | `.noinit`, `fault::Record` | 96 Б (всегда) |
| Отчёт об аварии | `-DMCFRAME_FAULT_REPORT=ON/OFF` | ON (OFF экономит ~400 Б) |
| Кольцо логов | `-DMCFRAME_LOG_ENTRIES=N` | 8 записей (~0.5 КБ) |
| Уровень логов | `-DMCFRAME_LOG_LEVEL=0..3` | 3 (Debug) |
| Статистика задачи | `task::runtime_ticks_`, `switches_` | +12 Б на TCB |
| Квант round-robin | `-DMCFRAME_TIME_SLICE_TICKS=N` | 0 (выключен) |
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

Ориентир для STM32F103RB (20 КБ RAM / 128 КБ Flash): пример с двумя задачами,
АЦП, самотестами, DMA-арбитром, супервизором энергосбережения, сторожевым
таймером и подсистемой OTA занимает **9.1 КБ RAM (45 %) и 67.3 КБ Flash (53 %)**
при сборке с LTO (`./build.sh stm32 Release`); без OTA те же подсистемы дают
5.4 КБ RAM и 34.6 КБ Flash — разницу стоит учитывать при выборе размера
слота — цифры берутся из отчёта линкера
(`--print-memory-usage`), а не из оценки. Состав статики виден в отчёте
`user_app_report_ram()`: `scheduler::static_bytes()`,
`g_memory_manager.static_bytes()`, `config::OsTimer::static_bytes()`,
`power::Supervisor::static_bytes()`, `watchdog::Service::static_bytes()`.

## Энергосбережение: супервизор сна и статистика нагрузки

Модуль `os/power/` управляет простоем ядра и измеряет загрузку CPU.
Смысл — в цифрах даташита (DS5319, типовые значения при 3.3 В):

| Состояние | Ток | Что его будит | Выход |
|---|---|---|---|
| Run 72 МГц (периферия выкл. / вкл.) | 27 / 36 мА | — | — |
| Run 8 МГц HSI | 4.0 / 4.9 мА | — | — |
| Sleep 72 МГц (ядро стоит, тактирование идёт) | 5.5 / 14.4 мА | **любое** прерывание | 1.8 мкс |
| **DeepSleep: STOP, LPDS=1** | **14 мкА** | EXTI-линия (в т.ч. будильник RTC) | 5.4 мкс |
| STOP, LPDS=0 | 24 мкА | то же | 3.6 мкс |
| STOP + RTC на LSE | ≈15 мкА (14 + 1.4 IDD_VBAT) | будильник RTC | 5.4 мкс |
| Standby | 2 мкА | WKUP/RTC | 50 мкс (SRAM теряется) |

Отсюда вся политика: **экономит не медленная работа, а выключенное ядро**.
Sleep выигрывает 5–30 раз по току, STOP — три порядка, и цена входа в STOP
(5.4 мкс) на фоне секунд простоя не значит ничего.

### Как этим пользоваться

```cpp
#include "os/power/power.hpp"

using namespace embed;

power::Config cfg{};
cfg.window_ms          = 1000u;    // окно измерения нагрузки
cfg.allow_deep_sleep   = true;     // разрешить STOP (нужен источник пробуждения)
cfg.deep_sleep_min_ms  = 8u;       // порог окупаемости входа в STOP
cfg.deep_sleep_max_ms  = 60000u;   // просыпаться не реже раза в минуту

if (power::Supervisor::instance().init(cfg) != Status::Ok) return 1;

// Хук супервизора — ПЕРВЫМ: он должен увидеть, что исполнялось в момент
// прерывания, до того как планировщик в этом же тике сменит задачу.
OsTimer::RegisterSystemHook(&power::Supervisor::tick_hook, nullptr);
OsTimer::RegisterSystemHook(&scheduler::tick_hook, nullptr);
```

Больше делать ничего не нужно: планировщик сам отдаёт простой супервизору
(его idle-задача вызывает `Supervisor::on_idle()`), а `init()` обязан
выполняться **до** `OsTimer::Start()` — смена профиля частоты на ходу
запрещена (см. ниже).

Что где применяется:

| Настройка | Зачем |
|---|---|
| `window_ms` | длина окна измерения нагрузки; по закрытию считается `load_percent` |
| `allow_deep_sleep` | разрешение STOP; без него платформа получает только WFI |
| `deep_sleep_min_ms` | порог окупаемости: короткий простой в STOP не выгоднее WFI |
| `deep_sleep_max_ms` | верхняя граница одного сна (сторожевой таймер, самопроверка) |
| `Config::profile` | статический профиль частоты (Performance / LowPower) |
| `set_wake_hint_ms(ms)` | «разбуди не позже чем через ms» — для внешних событий |
| `keep_awake()` | удержание: пока жив `Lease`, STOP запрещён |

### Статистика нагрузки

Загрузка измеряется **бесплатно**: в прерывании тика супервизор сравнивает
указатель текущей задачи с idle-задачей. Никаких счётчиков тактов, никакой
аппаратной поддержки — цена измерения один тик точности.

| Поле `power::Stats` | Что означает |
|---|---|
| `load_percent` / `peak_load_percent` / `avg_load_percent` | доля тиков, когда ядро исполняло задачи (в окнах бодрствования) |
| `window_ms` / `busy_ms` / `idle_ms` | текущее (незакрытое) окно измерения |
| `sleep_entries` | сколько раз останавливали только ядро (WFI) |
| `deep_sleep_entries` / `deep_sleep_ms` | сколько раз и как долго ядро было выключено полностью (STOP) |
| `refused_short` / `refused_keep_awake` / `refused_no_source` / `deep_sleep_failed` | почему глубокого сна НЕ было |

Последняя строка — не украшение: без причин отказов «мы не спим» выглядит
как ошибка супервизора, хотя обычно это незакрытый обмен по шине, слишком
короткий простой или невооружённый источник пробуждения. Готовый отчёт
печатает `user_app_report_power()` в примере.

### Почему супервизор НЕ переключает частоту по нагрузке

Профиль частоты выбирается **один раз** при инициализации (`Config::profile`),
а статистика нагрузки служит измерительным прибором для этого выбора — но не
контуром регулирования. Причины, по которым runtime-DVFS на STM32F1 не
делается:

* **Напряжение ядра не регулируется.** В `PWR_CR` STM32F103 нет битов VOS
  (только DBP/PLS/PVDE/CWUF/PDDS/LPDS), внутренний стабилизатор всегда даёт
  1.8 В. Энергия на переключение ≈ C·V², поэтому энергия **на операцию** от
  частоты не падает. Прямой расчёт по даташиту: 27 мА × 3.3 В при 72 МГц —
  это 1.24 нДж на цикл, а 4.0 мА × 3.3 В при 8 МГц — **1.90 нДж на цикл**,
  то есть на низкой частоте та же работа стоит на ~50 % дороже: статические
  токи «размазываются» на меньшее число тактов.
* **Выгоднее «добежать и уснуть».** 1 мс работы на 72 МГц + 99 мс STOP дают
  средний ток в разы меньше, чем 9 мс на 8 МГц + 91 мс STOP: работа
  выполняется, пока ядро всё равно включено, а простой уходит в микроАмперы.
  Снижение частоты вместо сна проигрывает даже по току: Sleep 72 МГц — это
  5.5 мА, а Run 8 МГц — 4.0 мА плюс обязательная работа периферии (~9 мА при
  72 МГц её включение добавляет к Run).
* **Переключение дорого.** Запуск кварца HSE — около 2 мс, захват PLL —
  до 200 мкс, а смена SYSCLK ломает всё, что считается от PCLK: делители
  baud у USART, `CCR` у I2C, прескалер АЦП (ADCCLK ≤ 14 МГц), SysTick
  (время ОС теряет опору). Периферию пришлось бы останавливать и
  перенастраивать на каждом переключении, а обмен по шинам — терять.
* **Ломается предсказуемость.** Задача, которой нужны миллисекунды, на
  пониженной частоте их не получает: время реакции становится функцией
  нагрузки, а не приоритета.

Поэтому в API есть ровно два профиля и одна чистая функция:

```cpp
// Что будет с нагрузкой на другой частоте (нагрузка ∝ 1/частота).
uint32_t at8 = power::project_load_percent(stats.peak_load_percent,
                                           supervisor.sysclk_hz(), 8000000u);
if (at8 <= 100u) { /* LowPower влезает: 8 МГц + непрерывная работа */ }
else             { /* только Performance, экономить — глубоким сном */ }
```

Правило простое: если пик нагрузки на 72 МГц укладывается в ~10 %, профиль
LowPower (8 МГц, HSI, PLL погашен) может быть оправдан — но только там, где
система обязана работать непрерывно. Во всех остальных случаях правильный
ответ — Performance плюс глубокий сон: замерьте `deep_sleep_ms` в отчёте,
это и есть реальный рычаг.

### Глубокий сон на STM32F1: что требуется

STOP включается флагом сборки `-DMCFRAME_STM32_DEEP_SLEEP=ON` (по умолчанию
**выключен**: уход в STOP без проверенного источника пробуждения не лечится
ничем, кроме Reset) и вызовом платформенной инициализации **до** старта тика:

```cpp
#include "power_stm32.hpp"

stm32::power::Config pcfg{};
pcfg.profile            = platform::PowerProfile::Performance;
pcfg.deep_sleep_enabled = true;
pcfg.rtc_clock          = stm32::power::RtcClock::Lsi;   // LSE точнее, LSI есть всегда
stm32::power::init(pcfg);            // до OsTimer::Init/Start
```

Что делает платформа, когда супервизор разрешил глубокий сон:

1. программирует будильник RTC на `now + запрошенный горизонт` (округление
   вверх до секунды) и разрешает пробуждение по линии EXTI17;
2. **останавливает SysTick** — иначе первое же прерывание тика (1–10 мс)
   вывело бы ядро из STOP и сна не получилось бы;
3. гасит тактирование (`PWR_CR` LPDS/PDDS, `SCB->SCR.SLEEPDEEP`, `WFI`);
4. после выхода снимает флаги, восстанавливает тактирование (`SystemInit()`
   возвращает 72 МГц — после STOP ядро работает от HSI) и системный тик;
5. возвращает **измеренное** по RTC время сна.

Последний пункт — то, ради чего всё затевалось: SysTick проспанное не считал,
поэтому ядро продвигает монотонное время (`OsTimer::AdvanceMs`) и «догоняет»
задержки спящих задач (`scheduler::advance_ticks`). Без этого `sleep_for(100)`
после минутного STOP ждал бы ещё 100 тиков, а таймауты «зависали».

Ограничения, о которых нужно знать:

* **Разрешение будильника RTC — одна секунда.** Просыпание может оказаться
  позже запрошенного (до +1 с), поэтому супервизор добавляет гранулярность к
  порогу: STOP используется только при простое от ~1 с. Если нужна точность
  до миллисекунд — это Sleep, а не STOP.
* **LSE запускается 1.5–2.5 с** (кварц 32.768 кГц); при `RtcClock::Lse`
  первый `init()` может занять секунды — это поведение кварца, а не
  зависание. LSI стартует за 85 мкс, но уезжает по частоте на десятки
  процентов (время сна измеряется по RTC, поэтому «секунда» LSI — не
  секунда).
* **В STOP не работает ничего, что считает от тактирования.** Незавершённый
  обмен UART/I2C/DMA не продвинется: задача будет ждать вечно. Поэтому
  асинхронный ввод-вывод берёт удержание сам (`async::kernel::KeepAwake` в
  `completion::wait`), а прикладной код делает так же вокруг своих ожиданий
  железа:
  ```cpp
  power::Supervisor::Lease lease = power::Supervisor::instance().keep_awake();
  // ... ждём периферию: STOP в это время запрещён ...
  ```
  Задачи, которые просыпаются часто (`sleep_for(1)` в цикле ожидания АЦП),
  блокируют STOP автоматически: горизонт равен одному тику, а он меньше
  порога окупаемости.
* **Пробуждение по внешнему событию** (кнопка, датчик на EXTI-линии) — это
  прикладная настройка линии; ядру достаточно `set_wake_hint_ms()` чтобы
  знать, что система должна проснуться.
* Код STOP **собран для STM32 и покрыт хостовым тестом на модели регистров,
  но на реальной плате не измерялся**: перед использованием проверьте ток
  потребления и выход из STOP на своей плате.

## Сторожевой таймер

Модуль `os/watchdog/` превращает зависание в перезагрузку — и делает это так,
чтобы после перезагрузки было понятно, ПОЧЕМУ она случилась.

### Почему «участники», а не просто мигать IWDG

Наивная схема — одна задача раз в секунду пишет в `IWDG_KR` — защищает только
от полного отказа: если зависла ЛЮБАЯ другая задача, а «кормилец» жив,
устройство не перезагрузится никогда. Здесь модель другая:

```
задача A ──check_in()──┐
задача B ──check_in()──┤   тик ──► сервис ──► все живы? ──да──► IWDG_KR = 0xAAAA
задача C ──check_in()──┘                      │
                                              нет ──► кормление прекращается
                                                      │
                                     аппаратный таймер ──► СБРОС устройства
```

* `Participant::init("sensor", 500)` — регистрация того, что обязано работать;
* `Participant::check_in()` — в КОНЦЕ успешного цикла, а не в начале;
* сервис в прерывании тика проверяет сроки всех участников и кормит железо
  только если живы все;
* решение **латчируется**: одна просрочка = сброс. Сторож, который «прощает»
  опоздание, маскирует растущую проблему — счётчик опозданий растёт, а
  устройство внешне работает как ни в чём не бывало.

Приложение может вмешаться осознанно: `Participant::fail()` («я больше не
работаю» — не дожидаясь срока) и `Service::rearm()` («я сам перезапустил
подсистему, продолжаем без сброса»). Второе — единственный способ отменить уже
принятое решение, поэтому вызовы считаются (`Stats::rearms`) и должны быть
видны в отчёте: «за смену подмигнули сторожу N раз» — это диагноз.

### Как этим пользоваться

```cpp
#include "os/watchdog/watchdog.hpp"

using namespace embed;

watchdog::Participant sensor_wd;      // статические объекты приложения
watchdog::Participant link_wd;

// 1. Настройка сервиса (до регистрации участников и до старта тика).
watchdog::Config cfg{};
cfg.mode            = watchdog::Mode::Warning;   // успеть сообщить ДО сброса
cfg.check_period_ms = 100u;                      // задержка обнаружения
cfg.max_stall_ms    = 2000u;                     // разрешённые длительные операции
cfg.on_warning      = &my_warning;               // короткий колбэк из прерывания
if (watchdog::Service::instance().init(cfg) != Status::Ok) return 1;

// 2. Участники: срок — с запасом к периоду задачи, иначе сторож станет
//    источником ложных сбросов.
sensor_wd.init("sensor", 500u);
link_wd.init("link", 1000u);

// 3. Хук в системный тик: если тик умрёт, кормить таймер станет некому.
OsTimer::RegisterSystemHook(&watchdog::Service::tick_hook, nullptr);

// 4. В КОНЦЕ инициализации приложения: с этого момента сторож необратим.
watchdog::Service::instance().start();

// 5. В задачах — отметка в конце успешного цикла:
void sensor_task(void*) noexcept {
    for (;;) {
        read_sensor();
        (void)sensor_wd.check_in();      // «цикл отработал»
        scheduler::instance().sleep_for(100);
    }
}
```

| Поле `Stats` | Что означает |
|---|---|
| `participants` / `checks` / `kicks` | сколько участников, проверок сроков и кормлений |
| `misses` / `warnings` / `max_late_ms` | просрочки, предупреждения и худшее опоздание |
| `early_checks` | отметки раньше `min_interval_ms` (задача «крутится», но работу не делает) |
| `stalls` / `refused_stalls` | разрешённые и отклонённые длительные операции |
| `deep_sleeps` / `slept_ms` | сколько раз и как долго система спала под сторожем |
| `rearms` | сколько раз приложение снимало защёлку просрочки |

### Почему IWDG, а не WWDG

| | IWDG (независимый) | WWDG (оконный) |
|---|---|---|
| Тактирование | LSI, свой генератор | PCLK1 (шина) |
| В STOP (глубокий сон) | **считает** | останавливается → защиты нет |
| При смене профиля частоты | период не меняется | период меняется вместе с шиной |
| Выключение | невозможно (только сброс питания) | возможно |
| Окно «не раньше срока» | нет | есть |

Отсюда выбор: для устройства, которое большую часть времени спит, годится
только независимый таймер — оконный в глубоком сне просто перестаёт защищать.
Роль окна («отметился слишком рано») играет `Participant::min_interval_ms`:
слишком частая отметка — такой же признак беды, как слишком редкая,
и в отличие от WWDG она называет виновника.

### Разброс LSI и почему таймаут считается «по худшему»

LSI на STM32F103 — внутренний RC на 30–60 кГц (типовое 40 кГц, DS5319).
Таймаут IWDG равен `(4 × 2^PR × RLR) / f_LSI`, поэтому расчёт ведётся по
**максимальной** частоте: обещанный срок — гарантированный минимум, а на
«типовых» 40 кГц он окажется примерно в 1.5 раза длиннее. Обратный выбор
(считать по 40 кГц) давал бы сбросы РАНЬШЕ срока, то есть ложные
срабатывания посреди нормальной работы. Границы, которые из этого следуют:

* минимальный срок — десятки микросекунд, максимальный — около 17 с;
* запрос большего срока даёт честный `NotSupported` от `Service::start()`,
  а не «поставим что получится».

### Связь с энергосбережением (важно)

Аппаратный таймер считает и в глубоком сне — а кормить его там **некому**.
Поэтому сервис регистрируется у супервизора энергосбережения как источник
ограничения сна (`power::Supervisor::add_sleep_limit`) и сообщает, сколько
система имеет права не просыпаться:

```
аппаратный таймаут = max(сроки участников) + запас
запас >= 2 × период проверки + запас на пробуждение
сон <= аппаратный таймаут − (период проверки + гранулярность источника пробуждения)
```

Отсюда практическое правило: если источник пробуждения — будильник RTC с
секундным разрешением, срок сторожа должен быть заведомо больше секунды,
иначе глубокий сон будет запрещён (и это правильное поведение: уснуть на
секунду, имея сторожа на 300 мс, означает гарантированный сброс).

После пробуждения сервис получает `on_slept(slept_ms)`: он немедленно кормит
таймер и начинает отсчёт сроков участников заново — во сне отметиться было
некому, и без этого первый же тик после сна обнаружил бы «просрочку» у всех.
Если защёлка уже стояла, сон её НЕ снимает: иначе глубокий сон стал бы
способом переждать нарушение.

### Посмертная диагностика: почему перезагрузилось

Отметка о просрочке пишется в резервные регистры (`BKP_DR*`), которые
переживают сброс, — 4 регистра начиная с `Config::note_base`:

| Смещение | Что хранится |
|---|---|
| `kNoteIdxMagic` | признак `'WD'`, что регистры наши |
| `kNoteIdxBoot` | номер текущей загрузки (счётчик перезагрузок) |
| `kNoteIdxFault` / `kNoteIdxLate` | индекс провинившегося участника + 1 и опоздание, мс |
| `kNoteIdxFBoot` | в какой загрузке случилась просрочка |

Следующая загрузка читает это в `Service::init()`, а приложение забирает
результат через `Service::last_fault()`:

```cpp
const watchdog::Fault f = watchdog::Service::instance().last_fault();
if (f.valid) {
    // Причина: сторож (или что-то ещё), виновник (если успели записать)
    // и номер загрузки. Это то, что уходит в журнал/на сервер.
}
watchdog::Service::instance().clear_fault();
```

Две тонкости, заложенные в разметку:

* отметка помечается номером загрузки, поэтому старая запись не «объясняет»
  все последующие перезагрузки;
* если причина сброса (RCC_CSR) — сторож, а отметки нет, `valid` всё равно
  `true` с `participant == kNoParticipant`: значит, записать не успели —
  например, не работало само прерывание тика. Молчать об этом нельзя.

### Ограничения, о которых нужно знать

* **Длительные операции.** Стирание Flash или калибровка блокируют тик дольше
  срока участника. Для них есть `allow_stall(ms)` — окно ОГРАНИЧЕНО
  `Config::max_stall_ms` и истекает само, поэтому забытая пауза не превращается
  в «сторож выключен навсегда», а каждый случай виден в статистике
  (`stalls`/`refused_stalls`). Пока окно открыто, глубокий сон запрещён.
* **Отметка не должна быть «попутной».** `check_in()` в начале цикла вместо
  конца превращает сторожа в формальность: задача, упавшая внутри цикла,
  успевает отметиться.
* **Регистрация участников — до `start()`**: аппаратный таймаут считается по
  их срокам один раз (после старта регистрация возвращает `Busy`).
* **IWDG не выключается.** `Service::stop()` на STM32 честно возвращает
  `NotSupported`: «выключить сторожа на время» почти всегда означает «выключить
  защиту там, где она нужнее всего». Для стенда есть `Mode::Off` — сервис
  следит и предупреждает, но железо не трогает.
* **Код собран для STM32 и покрыт хостовым тестом на модели регистров**
  (`IWDG_KR`/`IWDG_SR`, `BKP`, `RCC_CSR`), но на реальной плате не проверялся:
  перед использованием убедитесь, что ваша плата действительно перезагружается
  по сторожу (например, `while(1);` в задаче) и что таймаут вас устраивает.

## Диагностика отказов: Fault, panic, assert

Раньше `HardFault_Handler`/`MemManage`/`BusFault`/`UsageFault` были слабыми
алиасами на `Default_Handler`, то есть `for(;;)`: на плате это «зависло» без
PC, задачи и причины. Теперь есть `os/fault/`:

* снимает кадр исключения (`R0-R3/R12/LR/PC/xPSR`) и `CFSR/HFSR/MMFAR/BFAR`;
* включает `MEMFAULTENA/BUSFAULTENA/USGFAULTENA` при старте планировщика,
  поэтому MemManage/BusFault/UsageFault не «схлопываются» в HardFault;
* складывает запись в `.noinit` (переживает reset, но не потерю питания) и
  короткую метку в `BKP_DR10` (переживает и потерю питания);
* печатает строку `MCFrame fault: ... pc=... lr=... cfsr=...`;
* перезагружает MCU через `platform::ops().reset()` — а не крутится вечно.

### Как пользоваться

```cpp
#include "os/fault/fault.hpp"

int main() {
    (void)embed::fault::init();          // прочитать запись после reset
    if (embed::fault::was_fault_reset()) {
        embed::fault::report();          // или показать в shell
        embed::fault::clear();
    }

    MCFRAME_ASSERT(cfg.baudRate != 0u);  // запись + reset вместо std::assert
    // ...
}
```

`fault::last()` отдаёт `Record` (PC, LR, CFSR, регистры, имя задачи, файл и
строку); `fault::clear()` стирает запись и BKP-метку. `MCFRAME_PANIC(kind)` —
безусловная авария.

### Стоимость и режимы

| Что | Где | По умолчанию |
|---|---|---|
| Полный отчёт (код + строки) | `-DMCFRAME_FAULT_REPORT=ON/OFF` | ON (OFF — если не хватает места) |
| Подробный дамп регистров | `-DMCFRAME_FAULT_VERBOSE=1` | OFF |
| Запись в `.noinit` | `linker_sections.ld`, 96 Б RAM | всегда |
| BKP-метка | `fault::kNoteIndex = 9` (`BKP_DR10`) | всегда |

Полный отчёт стоит ~400 Б Flash. По умолчанию он включён и в `flat`, и в
`slot` (минимальный пример собирается с `-Os`). Если ваше приложение
переполняет слот, соберите `-DMCFRAME_FAULT_REPORT=OFF`: запись об аварии и
сброс останутся, печать — нет; приложение сможет само вызвать
`fault::last()` и напечатать поля.

### Переполнение стека

Дно каждого стека хранит `kStackGuardMagic` (`0xDEADBEEF`). В
`prepare_context_switch()` уходящая задача проверяется **до** сохранения
контекста: если маркер затёрт, `fault::stack_overflow(task)` пишет запись с
именем задачи и перезагружает MCU. Это не замена MPU (у STM32F103
medium-density его нет), а дешёвая канарейка: она фиксирует факт порчи, но не
предотвращает её. Проверить вручную можно через `scheduler::stack_guard_ok(t)`.

### Ограничения

* После **потери питания** `.noinit` теряется; в BKP остаётся только тип
  аварии (не PC). Для полного разбора после пропадания питания нужен внешний
  лог или запись во Flash.
* В схеме с загрузчиком `.noinit` — это область RAM конкретного образа; при
  переходе загрузчик→приложение адрес может отличаться. Тип аварии переживает
  переход в BKP, полный PC — только если образы не перекрыли область друг
  друга. Для гарантированного полного разбора храните запись в отдельной
  странице Flash или в BKP-кольце.
* BKP-регистры расписаны: 0..4 — watchdog (`kNoteCount = 5`), 5..8 — OTA
  (`note_base = 5`), 9 — авария. Если меняете `note_base` OTA, не займите 9.
* Обработчик не пытается продолжить исполнение и не использует malloc/Lock:
  после Fault состояние ядра не определено.
* Тесты на стенде проверяют запись и отчёт на синтетическом кадре; сам переход
  в исключение проверяется только на железе.

## Логирование

`os/log/` — уровни, метка, кольцевой буфер и безопасная запись из ISR:

```cpp
#include "os/log/log.hpp"

MCFRAME_LOG_INFO("APP", "started");
embed::log::write_u32(embed::log::Level::Warn, "OTA", "offset", 4096u);

// Из прерывания: только в кольцо, без вывода (ITM/UART могут ждать)
embed::log::from_isr(embed::log::Level::Error, "UART", "overrun");
```

| Настройка | Где | По умолчанию |
|---|---|---|
| Размер кольца | `-DMCFRAME_LOG_ENTRIES=N` | 8 записей |
| Компиляторный порог | `-DMCFRAME_LOG_LEVEL=0..3` | 3 (Debug) |
| Уровень на ходу | `log::set_level()` | Info |

`log::pop()` отдаёт записи по одной; shell-команда `log` печатает кольцо.
`from_isr()` не вызывает `platform_log_line`, потому что вывод в ITM/UART может
ждать отладчик/линию — в прерывании это недопустимо.

Модули уже подключены к логу: OTA пишет старт/проверку/подпись/активацию,
watchdog — пропуск участника через `from_isr`, power — отказ глубокого сна.
Поэтому `shell log` показывает не только сообщения приложения, но и события
подсистем.

## Shell и статистика задач

`os/shell/` — консоль, не знающая про UART: приложение кормит её символами и
задаёт writer. Встроенные команды: `help`, `ps`, `free`, `stats`, `log`,
`reboot`.

```cpp
#include "os/shell/shell.hpp"
#include "os/shell/shell_watchdog.hpp"   // опционально: команда wd
#include "os/shell/shell_ota.hpp"        // опционально: команда ota

static void shell_out(void* ctx, const char* s) noexcept {
    auto* uart = static_cast<UartPort*>(ctx);
    (void)uart->write(s);                 // или platform_log_line
}

embed::shell::Console sh;
sh.init(&shell_out, &uart);
embed::shell::add_watchdog_commands(sh);  // тянет watchdog только если вызвано
embed::shell::add_ota_commands(sh);       // то же для OTA

// в задаче:
uint8_t b;
if (uart.read(&b, 1u, 10u) == Status::Ok) sh.feed(static_cast<char>(b));
```

`ps` печатает имя, состояние, приоритет/base, `runtime_ticks`, долю CPU,
число диспетчеризаций и свободные слова стека. Для этого у `task` есть
`runtime_ticks()`/`switches()`, а у планировщика — `task_snapshot()`: снимок
указателей делается под Lock, а печать идёт уже без него, поэтому shell не
блокирует планировщик на время вывода.

`wd`/`ota` вынесены в отдельные заголовки: приложение, которому они не нужны,
не тянет их код в образ. `ps/free/stats/log/reboot` живут в ядре и всегда
доступны, если shell слинкован.

В примере shell включается флагом `-DMCFRAME_APP_SHELL=ON` (flat). При этом
UART1 отдаётся shell, а транспорт OTA отключается: два читателя одного потока
не могут делить MOLP-кадры. В slot эта комбинация запрещена явной ошибкой
CMake — 52 КБ не хватает на OTA + shell.

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

## OTA: приём новой прошивки

Модуль `os/ota/` принимает новый образ прошивки, записывает его в носитель,
проверяет и оставляет метки «что грузить». Он отвечает на три вопроса, которые
обычно и решают судьбу обновления:

1. **ГДЕ** хранится новая прошивка (`Storage` + раскладка слотов);
2. **ПО ЧЕМУ** она принимается (`Transport`: UART/USB/память/никак);
3. **ЧТО** считается принятым (протокол MOLP v1: кадры, проверки, состояния).

### Чего модуль НЕ делает (и почему это осознанно)

* **Не переносит управление на новую прошивку.** На STM32F103 это работа
  загрузчика: перенести SP, взять вектор сброса, прыгнуть. Сделанный
  «наполовину» из приложения, такой переход даёт устройство, которое не
  грузится вовсе. Модуль готовит слот и оставляет метки (состояние слота,
  версия, число попыток загрузки), по которым загрузчик решает, что грузить.
* **Не шифрует образ.** Целостность (CRC32, SHA-256) и аутентичность
  (подпись) — да, конфиденциальность — нет. Проверку подписи выполняет
  приложение (`Config::verify_signature`), модуль лишь передаёт ему ровно то,
  что было подписано, и не «верит флагу в кадре».
* **Не выделяет память.** Буферы кадров, состояние сессии и контекст хеша —
  статические поля сервиса: `ota::Service::static_bytes()` (≈1.4 КБ при
  настройках по умолчанию) известен на этапе сборки.

### ГДЕ хранится новая прошивка

Работа с носителем идёт через абстракцию `ota::Storage`, поэтому одна и та же
логика приёма, проверки и активации работает и на встроенной Flash STM32, и на
внешней микросхеме, и на буфере в памяти:

| Носитель | Класс | Что это |
|---|---|---|
| Встроенная Flash | `ota::InternalFlashStorage` | Поверх контракта `platform::FlashOps` (`platforms/stm32/flash_stm32.cpp`: unlock, стирание страницы, half-word). Раскладка слотов — в верхней части Flash, чтобы слот не накрывал исполняемый код |
| Внешняя память | `ota::ExtRomStorage` | Поверх `platform::ops().ext_rom_*` (SPI-flash, SD, FRAM — что именно, решает платформа) |
| Буфер в памяти | `ota::MemoryStorage` | ОЗУ, внешняя RAM, буфер стенда. Годится и для сценария «принять в память, записать во Flash потом» |
| Своё | наследник `ota::Storage` | Достаточно реализовать `capacity/page_size/erase/write/read` |

Раскладка задаётся конфигурацией, а не «догадкой кода»:

```
offset 0              meta_offset          slot_offset[0]   slot_offset[1]
│                     │                    │                │
│  (что угодно —      │ 4 страницы мета-   │ образ A        │ образ B
│   например код,     │ данных: по 2       │ (слот)         │ (слот)
│   если носитель     │ копии на слот      │                │
│   общий с кодом)    │ (рабочая + резерв) │                │
```

```cpp
ota::Config cfg{};
cfg.storage          = &g_flash;      // ГДЕ: встроенная Flash
cfg.meta_offset      = 96u * 1024u;   // 4 страницы метаданных
cfg.slot_offset[0]   = 100u * 1024u;  // слот A
cfg.slot_capacity[0] = 12u * 1024u;
cfg.slot_offset[1]   = 112u * 1024u;  // слот B
cfg.slot_capacity[1] = 12u * 1024u;
cfg.active_slot      = 0u;            // исполняемся из слота A
```

`init()` **проверяет** раскладку и отказывает (`InvalidArg`), если слот
перекрывает метаданные, слоты перекрывают друг друга или носитель не имеет
страниц. Ошибка в этих числах иначе проявилась бы посреди передачи — то есть
тогда, когда слот уже стёрт.

**Почему метаданные — отдельные страницы, а не заголовок образа.** Состояние
(сколько байт принято и какая CRC на этой длине) сохраняется несколько раз за
передачу — именно оно позволяет продолжить обрывок после потери питания. На
NOR-flash перезаписать байт нельзя: сначала стирается страница. Заголовок в
начале образа означал бы, что каждое сохранение состояния стирает уже принятые
данные. Поэтому метаданные живут в своих страницах, а образ пишется строго
вперёд, один раз на байт до следующего стирания слота.

**Почему копий записи две.** Стирание и запись не атомарны: сбой питания между
ними оставил бы «полустёртую» запись, и о состоянии не узнал бы никто. Две
копии со счётчиком `seq` решают это: пока пишется старшая копия, младшая
остаётся целой, а при чтении берётся та, у которой `seq` больше и CRC32 цела.

**Почему слот стирается постранично, по мере записи.** Стереть 60 КБ «одним
куском» — это 60 страниц по ~30 мс, то есть почти две секунды без кормления
сторожа. Сервис стирает только те страницы, до которых дошла запись, и на
каждую берёт у сторожа окно (`Config::erase_stall_ms`,
`watchdog::Service::allow_stall`).

### ПО КАКОМУ интерфейсу принимается прошивка

Протокол работает с потоком байт и не знает, что под ним. Интерфейс задаётся
одним полем:

| Интерфейс | Как задать | Когда |
|---|---|---|
| UART / USART / RS-485 / USB-CDC | `ota::StreamTransport<Драйвер>{Uart::instance(), proto::Iface::Uart}` | Основной путь: это потоки байт по своей природе |
| Любой свой порт | `ota::StreamTransport<Port>` — нужны только `read(buf,cap,n,ms)` и `write(data,len,ms)` | Экзотические интерфейсы: SPI-ведомый, CAN-мост, радиоканал |
| Буфер в памяти | `ota::MemoryTransport` | Стенд, тесты, «образ уже принят другим каналом» |
| Интерфейса нет | `cfg.transport = nullptr` | Образ залил программатор: устройству остаётся `VERIFY`/`ACTIVATE` |

Транспорт для сервиса — это `begin/read/write/iface/max_payload`. Если
интерфейс сообщает свой предел кадра (`max_payload`), сервис честно объявит
хосту МЕНЬШИЙ предел в INFO — иначе тот присылал бы кадры, которые некуда
положить.

Готового транспорта для **I2C-ведомого** в модуле нет намеренно: I2C — не
поток, обмен инициирует ведущий, поэтому кадр укладывается в транзакцию
(запись), а ответ уходит в следующую (чтение). Такой транспорт пишется
наследником `ota::Transport` поверх `II2c<Impl>` и сообщает `Iface::I2c`; весь
протокол выше от этого не меняется.

### Протокол приёма: MOLP v1 (MCFrame OTA Link Protocol)

Полное описание — в `src/os/ota/ota_protocol.hpp`. Ниже — то же самое, но
с точки зрения того, кто пишет хост-инструмент.

**Формат кадра.** Все многобайтные поля — little-endian, размер кадра = 13 + LEN.

| Смещение | Размер | Поле | Назначение |
|---|---|---|---|
| 0 | 1 | `SOF0` | `0x4F` ('O') |
| 1 | 1 | `SOF1` | `0x54` ('T') |
| 2 | 1 | `VER` | версия протокола (1) |
| 3 | 1 | `OP` | операция |
| 4 | 1 | `FLAGS` | флаги кадра |
| 5 | 1 | `SEQ` | номер кадра в направлении (8 бит, циклически) |
| 6..7 | 2 | `LEN` | длина полезной нагрузки |
| 8 | 1 | `HCRC` | CRC-8/SMBUS по байтам 2..7 |
| 9.. | LEN | `PAYLOAD` | полезная нагрузка |
| 9+LEN | 4 | `CRC32` | CRC-32/ISO-HDLC (zlib) по байтам 2..(8+LEN) |

Два CRC, а не один, потому что у них разные задачи. `HCRC` закрывает только
заголовок и проверяется ДО того, как приёмник поверит полю `LEN`: иначе
испорченная длина заставила бы его ждать несуществующие байты и терять
синхронизацию из-за одной ошибки в линии. `CRC32` закрывает весь кадр и
является окончательным приговором.

CRC32 — именно zlib-вариант (`zlib.crc32` в Python, `crc32` в coreutils,
`zlib` в C), а не аппаратный CRC-32/MPEG-2 блока CRC у STM32F1: сборщику
образа не нужна наша таблица, чтобы посчитать ожидаемое значение. Пресет
`crypto::Crc32Stm32` в библиотеке есть, если позже понадобится ускорение.

**Операции.**

| Код | Операция | Payload | Ответ |
|---|---|---|---|
| 0x01 | `INFO` | версия протокола хоста (1 байт) | `ACK` с полезной нагрузкой `Info` (32 байта) |
| 0x02 | `START` | `Start` (54 байта) | `ACK` (next_offset) / `NAK` |
| 0x03 | `DATA` | `offset(4)` + данные | `ACK` (next_offset) / `NAK` |
| 0x04 | `END` | `End` (40 байт) | `ACK` (состояние) / `NAK` |
| 0x05 | `SIGN` | `total_len(2) offset(2) kind(1)` + подпись | `ACK` / `NAK` |
| 0x06 | `ABORT` | причина (1 байт) | `ACK` |
| 0x07 | `ERASE` | `slot(2)` | `ACK` / `NAK` |
| 0x08 | `VERIFY` | `Verify` (44 байта) | `ACK` / `NAK` |
| 0x09 | `ACTIVATE` | `slot(2)` | `ACK` / `NAK` |
| 0x0A | `CONFIRM` | — | `ACK` |
| 0x0B | `STATE` | — | `ACK` |
| 0x0C | `READ` | `offset(4) len(2)` | `ACK` + данные |
| 0x80 | `ACK` | `Response` (28 байт) | — |
| 0x81 | `NAK` | `Response` (28 байт) | — |

`ACK` и `NAK` устроены одинаково, чтобы у хоста был один разбор ответа:
`code(1) state(1) slot(2) max_payload(2) detail(2) next_offset(4) image_crc32(4)
fw_version(4) slot_offset(4) slot_capacity(4)`.

В `INFO` устройство сообщает: версию протокола, вид интерфейса, свои
возможности (SHA-256, подпись, продолжение, два слота), предел полезной
нагрузки, размер страницы, версию работающей прошивки, смещения и ёмкости
слотов и предел размера образа.

**Обмен сообщениями.** Полудуплекс, строго «запрос — ответ», окно = 1: хост не
шлёт следующий кадр, пока не получил ответ (или не истёк таймаут). Для МКУ это
осознанный выбор: не нужны буферы под несколько кадров в полёте, а потеря
ответа стоит одного повтора.

```
хост                                       устройство
INFO         ────────────────────────────►  ACK(Info)          возможности
START        ────────────────────────────►  ACK(next=0)        сессия открыта
DATA(0)      ────────────────────────────►  ACK(next=124)      записано 124 Б
DATA(124)    ────────────────────────────►  ACK(next=248)
...                                        DATA с тем же offset — повтор:
                                           ACK с текущим next_offset
END(size,crc,sha) ───────────────────────►  ACK(Verified)      образ проверен
SIGN(kind,sig)    ───────────────────────►  ACK(Verified)      подпись принята
ACTIVATE(slot)    ───────────────────────►  ACK(Activated)     грузим при сбросе
CONFIRM           ───────────────────────►  ACK(Running)       прошивка
                                                              подтвердила себя
```

**Правила приёма** (обязательства устройства — и то, на что может опираться
хост):

1. **Кадр, не прошедший CRC, не обрабатывается.** Если удалось разобрать
   `SEQ` — устройство отвечает `NAK(BadFrame)`; отвечать на мусор оно не
   обязано. Такой `NAK` приходит ВНЕ ОЧЕРЕДИ (он не ответ на текущий запрос),
   и хост должен его игнорировать, если не ждёт ответа. Частота таких ответов
   ограничена (`Config::bad_frame_nak_period_ms`), поэтому «залить» ими канал
   нельзя.
2. **Синхронизация восстанавливается сдвигом на байт.** Приёмник не «съезжает»
   навсегда после одной ошибки: начало кадра сдвигается на байт, и те же байты
   разбираются заново, поэтому кадр, начинающийся внутри чужой полезной
   нагрузки, будет найден. Кадр с неправдоподобно большим `LEN` считается не
   «большим кадром», а испорченным заголовком (иначе он проглотил бы вместе с
   собой и все следующие кадры).
3. **Повтор `DATA` идемпотентен.** `offset < next_offset` — это ретрансмиссия:
   данные не перезаписываются, в ответ идёт текущий `next_offset`. Поэтому
   потерянный ответ стоит одного повтора, а не повторной записи.
4. **Разрыв обнаруживается по смещению.** `offset > next_offset` — `NAK`
   с `next_offset`, и хост продолжает с него. Потеря кадра не превращается в
   дыру в прошивке.
5. **Запись только вперёд.** Образ пишется строго последовательно — это
   требование NOR-flash (записать мимо стирания нельзя), и «принять блок 5,
   потом блок 3» не нужно: у хоста уже есть все байты.
6. **Непринятое не активируется.** Слот считается готовым только после `END`
   и проверки CRC32 (и SHA-256/подписи, если они заявлены).
7. **Обрыв связи — не ошибка, а пауза.** После `Config::session_timeout_ms`
   сессия закрывается, но в метаданных остаётся `stored_bytes` и CRC32 образа
   на этой длине. Следующий `START` с флагом `kStartResume` перечитывает
   РЕАЛЬНО лежащий префикс и продолжает с того, что проверено, — а не с того,
   что помнит хост.

**Выравнивание записи — не мелочь.** Носитель, который не умеет
программировать одиночный байт (встроенная Flash STM32F1: только half-word),
отвергает невыровненную запись, и это правильно: дописать половину уже
запрограммированной пары нельзя — изменение содержимого ячейки требует
стирания страницы. Поэтому сервис сам копит принятые байты и отдаёт носителю
только порции, кратные `write_granularity()`, а последнюю добивает значением
`0xFF` в свободной части слота. Отсюда два следствия, которые стоит знать:
образ ЛЮБОЙ длины (в том числе нечётной) заливается корректно, а число
«физически записано» в записи состояния может быть на одну гранулу больше
принятого — читать обратно имеет смысл именно его.

**Пределы, на которых легко ошибиться на 4 байта.** `INFO.max_payload` и
`Response.max_payload` — предел полезной НАГРУЗКИ КАДРА. Полезная нагрузка
`DATA` состоит из 4 байт смещения и самого блока, поэтому предел БЛОКА =
`max_payload - 4` (при 128 это 124 байта). Блок больше предела устройство
отвергает с `TooLarge` и сообщает в `detail` фактический предел блока.

**Состояния** (уходят в ответах и хранятся в записи слота):
`Idle → Started → Receiving → Complete → Verified → Activated → Running`,
плюс `Failed`. Готовым к загрузке считается только `Verified`, `Activated`
или `Running`.

**Коды отказа** (поле `code`): `BadFrame`, `BadState`, `BadOffset`, `NoSpace`,
`Storage`, `Verify`, `Busy`, `Unsupported`, `Auth`, `TooLarge`, `Timeout`,
`Denied`, `BadRequest`. Они разные не для красоты: по коду хост понимает, что
делать — повторить кадр, перемотать на `next_offset`, ждать или прекратить.

**Проверка образа — три независимых шага.**

1. `CRC32` — считается по принятым байтам и сверяется с заявленной в `START`;
2. `SHA-256` (если заявлен `HashKind::Sha256`) — ловит то, что CRC32 не ловит:
   подмену образа, а не порчу при передаче;
3. **Проверка чтением** (`Config::verify_readback`, по умолчанию включена) —
   образ ПЕРЕЧИТЫВАЕТСЯ из носителя и сверяется ещё раз. «`write()` вернул Ok»
   и «в памяти лежит то, что писали» — разные утверждения, и на дешёвой Flash
   второе иногда неверно. Именно эти три шага закрывают три разных отказа:
   линию, подмену и носитель.

**Подпись — отдельный шаг и отдельное состояние.** После `END` слот уже
целый, но ещё не готов к загрузке: при `Config::require_signature` состояние
остаётся `Complete`, и только `SIGN` с проверенной подписью переводит его в
`Verified`. Подписывается ровно это сообщение (52 байта):

```
"MCFRAMEO" | image_size(4) | image_crc32(4) | fw_version(4) | sha256(32)
```

Тег в начале защищает от подписи «похожего» сообщения из другого протокола, а
сама проверка выполняется приложением (`Config::verify_signature`) — модуль не
знает, ECDSA там, Ed25519 или HMAC, и не хранит ключей. Длинная подпись
(например RSA-2048) не влезает в один кадр и передаётся фрагментами:
`kFlagMore` у всех фрагментов, кроме последнего, а `next_offset` в ответе
говорит, сколько байт подписи уже принято.

**Что приёмник НЕ разрешит** (проверено тестами, и это не «политика вежливости»):

* писать в слот, из которого СЕЙЧАС исполняется код: стирание такой страницы —
  это HardFault. Ответ `Denied`; снимается только настройкой
  `allow_active_erase` (код работает из ОЗУ);
* затирать ПРОВЕРЕННУЮ копию другим образом без явного `kStartOverwrite`:
  это резерв на случай отката, и терять его молча нельзя;
* активировать слот, если требуется подпись, а её в слоте нет
  (`require_signature` + отсутствие флага проверки подписи) — иначе проверку
  подписи можно было бы обойти каналом `VERIFY`/`ACTIVATE`;
* выполнять `VERIFY` при `require_signature`: этот кадр подписи не несёт,
  поэтому пометить образ годным он не может (ответ `Denied`);
* применять `SIGN` вне сессии: подпись относится к конкретному слоту, и кадр,
  пришедший «сам по себе» (после перезагрузки), отвергается, а не переписывает
  запись состояния чужого слота;
* запускать `START` с нереализованным флагом `kStartVerifyOnly`: ответ
  `Unsupported`, а не «молча открыть обычную сессию со стиранием».

**Активация, пробная загрузка и откат.** `ACTIVATE` помечает проверенный слот
как «грузить при следующей перезагрузке» и записывает отметку в резервные
регистры (`Config::note_base`; по умолчанию 5 — регистры 0..4 заняты сторожем).
При следующем старте `boot_check()` отвечает на вопрос «эта загрузка
пробная?» и увеличивает счётчик попыток. Приложение, успешно
инициализировавшееся, вызывает `confirm_running()` — счётчик сбрасывается,
состояние становится `Running`. Если попытки исчерпаны
(`Config::max_boot_attempts`), `boot_check()` сообщает `rollback_needed`, и
загрузчик обязан вернуться к прежнему слоту (`rollback()` активирует другой
проверенный слот, а если такого нет — снимает активацию). Так прошивка,
которая валится на старте, не превращает устройство в «кирпич».

### Как залить прошивку с ПК

Протокол прост настолько, что хост-инструмент — это сотня строк на Python (в
репозитории его нет: у каждой команды свой канал и свой формат ключей, а
протокол — вот он). Скрипт ниже — рабочая реализация стороны отправителя:

```python
#!/usr/bin/env python3
"""Заливка прошивки по MOLP v1 (см. README, «OTA»). Требуется pyserial."""
import struct, sys, time, zlib
try:
    import serial
except ImportError:
    sys.exit("нужен pyserial: pip install pyserial")

SOF0, SOF1, VER = 0x4F, 0x54, 1
OP_INFO, OP_START, OP_DATA, OP_END, OP_ACTIVATE = 1, 2, 3, 4, 9
ACK, NAK = 0x80, 0x81
AUTO_SLOT = 0xFFFF

def crc8(data):                      # CRC-8/SMBUS (poly 0x07, init 0)
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else ((crc << 1) & 0xFF)
    return crc

def frame(op, seq, payload=b"", flags=0):
    head = struct.pack("<BBBBH", VER, op, flags, seq, len(payload))   # VER..LEN
    hcrc = bytes([crc8(head)])                                        # HCRC
    crc = zlib.crc32(head + hcrc + payload) & 0xFFFFFFFF              # CRC32
    return bytes([SOF0, SOF1]) + head + hcrc + payload + struct.pack("<I", crc)

def read_frame(port, timeout=3.0):
    """Читает один кадр, синхронизируясь по SOF. Возвращает (op, seq, payload)."""
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += port.read(64)
        start = buf.find(bytes([SOF0, SOF1]))
        if start < 0 or len(buf) - start < 13:
            continue
        head = buf[start + 2:start + 8]
        hcrc = buf[start + 8]
        if hcrc != crc8(head):            # заголовок испорчен — ищем дальше
            buf = buf[start + 3:]
            continue
        length = struct.unpack("<H", head[4:6])[0]
        total = 13 + length
        if len(buf) - start < total:
            continue
        body = buf[start + 2:start + 9 + length]
        if struct.unpack("<I", buf[start + 9 + length:start + total])[0] != zlib.crc32(body):
            buf = buf[start + 3:]
            continue
        return head[1], head[3], buf[start + 9:start + 9 + length]
    raise TimeoutError("ответ не получен")

def request(port, op, seq, payload=b"", flags=0):
    port.write(frame(op, seq, payload, flags))
    while True:
        r_op, r_seq, r_payload = read_frame(port)
        if r_op in (ACK, NAK):
            # посторонний NAK(BadFrame) — не ответ на наш запрос
            if r_op == NAK and r_payload and r_payload[0] == 1:
                continue
            code, state, slot, max_payload, detail, nxt, crc, fwver, off, cap = \
                struct.unpack("<BBHHHIIIII", r_payload[:28])
            return r_op == ACK, code, nxt, max_payload, state
        return False, 255, 0, 0, 0

def main():
    image = open(sys.argv[1], "rb").read()
    port = serial.Serial(sys.argv[2], 115200, timeout=0.05)
    seq = 0

    # 1. INFO: узнаём предел кадра и раскладку
    ok, code, _, max_payload, _ = request(port, OP_INFO, seq, bytes([VER]))
    seq = (seq + 1) & 0xFF
    assert ok, f"INFO отклонён: code={code}"
    block = max_payload - 4                     # 4 байта смещения внутри DATA

    # 2. START: размер, CRC32 образа, версия; слот — «выбери сам»
    start = struct.pack("<IIIHHBBI32s",                       # раскладка Start
                        len(image), zlib.crc32(image) & 0xFFFFFFFF, 0x00010001,
                        AUTO_SLOT, block, 0, 0, 0, b"\0" * 32)
    ok, code, next_offset, _, _ = request(port, OP_START, seq, start)
    seq = (seq + 1) & 0xFF
    assert ok, f"START отклонён: code={code}"

    # 3. DATA: блоки по block байт, с повтором по таймауту
    offset = next_offset
    while offset < len(image):
        chunk = image[offset:offset + block]
        payload = struct.pack("<I", offset) + chunk
        try:
            ok, code, next_offset, _, _ = request(port, OP_DATA, seq, payload)
        except TimeoutError:
            continue                            # ответ потерян — повторяем кадр
        seq = (seq + 1) & 0xFF
        if not ok:
            if code == 3:                       # BadOffset: разрыв — мотаем назад
                offset = next_offset
                continue
            raise SystemExit(f"DATA отклонён на {offset}: code={code}")
        offset = next_offset                    # верим УСТРОЙСТВУ, а не себе
        print(f"\r{offset * 100 // len(image)}%", end="")

    # 4. END: проверка целостности (SHA-256 не заявлен — нули)
    end = struct.pack("<II32s", len(image), zlib.crc32(image) & 0xFFFFFFFF, b"\0" * 32)
    ok, code, _, _, state = request(port, OP_END, seq, end)
    seq = (seq + 1) & 0xFF
    assert ok, f"END отклонён: code={code}"
    print(f"\nобраз проверен, состояние={state}")

    # 5. ACTIVATE: грузить при следующей перезагрузке
    ok, code, _, _, state = request(port, OP_ACTIVATE, seq, struct.pack("<H", AUTO_SLOT))
    print("активирован" if ok else f"активация отклонена: code={code}")

if __name__ == "__main__":
    main()
```

Что здесь стоит заметить: смещение продолжения хост берёт из ответа
(`next_offset`), а не считает сам, — при обрыве связи и при повторе это
единственный способ не оставить дыру в образе. Если в проекте требуется
подпись, между `END` и `ACTIVATE` добавляется `SIGN` с подписью того самого
52-байтного сообщения.

Ещё две вещи, которые хосту полезно знать. Первая: если слот занят
ПРОВЕРЕННЫМ образом (это резервная копия для отката), устройство отвечает
`Denied` — затирать запас молча нельзя, и повторный `START` шлётся с флагом
`kStartOverwrite` (`flags = 1 << 1`). Вторая: команда `ABORT` (0x06) закрывает
сессию, а `ERASE` (0x07) стирает слот целиком — она полезна, если хост решил
начать передачу заново и не хочет ждать постраничного стирания по ходу дела.

### Интеграция в приложение

```cpp
// 1. Инициализация (до старта планировщика): ГДЕ хранить и ПО ЧЕМУ принимать.
ota::Config cfg{};
cfg.storage = &g_flash; cfg.transport = &g_ota_uart;
cfg.meta_offset = 96u * 1024u; cfg.slot_offset[0] = 100u * 1024u; /* ... */
cfg.fw_version = FW_VERSION;                 // запрещает откат на старую версию
cfg.erase_stall_ms = 1000u;                  // окно сторожу на стирание страницы
(void)ota::Service::instance().init(cfg);

// 2. Задача приёма: сервис сам опрашивает интерфейс.
void ota_task(void*) noexcept {
    for (;;) {
        (void)ota::Service::instance().poll(50u);   // бюджет, мс
        (void)g_ota_wd.check_in();                  // участник сторожа
        (void)scheduler::instance().sleep_for(100);
    }
}

// 3. Загрузка: что грузим в этой загрузке и не пора ли откатиться.
ota::BootInfo bi{};
(void)ota::Service::instance().boot_check(bi);
if (bi.valid && bi.rollback_needed) (void)ota::Service::instance().rollback();
// ... успешная инициализация приложения ...
(void)ota::Service::instance().confirm_running();   // «я запустилась и жива»
```

Сервис сам держит систему от глубокого сна, пока открыта сессия
(`Config::keep_awake`): в STOP гаснет тактирование интерфейса, и приём
«замирает». На длительное стирание он берёт у сторожа окно
(`Config::use_watchdog_stall`), а если у приложения есть свой участник
сторожа — отмечаться в задаче приёма нужно самому (как в примере).

### Стоимость и настройки сборки

| Что | Где | По умолчанию |
|---|---|---|
| Предел полезной нагрузки кадра | `-DMCFRAME_OTA_MAX_PAYLOAD=N` | 128 байт (буфер приёмника — 141 Б) |
| Предел подписи | `-DMCFRAME_OTA_SIGNATURE_MAX=N` | 80 байт |
| Предел «проглатываемого» кадра | `-DMCFRAME_OTA_MAX_SKIP=N` | 1024 байта |
| Буфер транспорта в памяти | `-DMCFRAME_OTA_MEMORY_BUFFER=N` | 2048 байт |
| Период сохранения состояния | `Config::checkpoint_bytes` | 4096 байт |
| Таймаут сессии | `Config::session_timeout_ms` | 30000 мс |

Статика сервиса при настройках по умолчанию — **≈1.5 КБ**
(`ota::Service::static_bytes()`): буферы приёма и ответа, буфер проверки
чтением, две записи состояния на слот, контекст SHA-256. В примере включение
OTA (вместе с SHA-256 и CRC32, которые до этого не линковались) поднимает
прошивку с 34.6 КБ Flash / 5.4 КБ RAM до **67.3 КБ Flash (53 %) / 9.1 КБ
RAM (45 %)** на STM32F103RB при сборке с LTO — цифры из отчёта линкера, а не
из оценки.

### Ограничения, о которых нужно знать

* одна сессия на устройство: второй `START` во время передачи получает `Busy`
  (повтор того же `START` — это идемпотентный ответ с текущим `next_offset`);
* окно передачи = 1: скорость ограничена задержкой «кадр — ответ». На 115200
  бод блок 124 байта — это 141 байт кадра (≈12 мс) плюс 41 байт ответа (≈4 мс),
  то есть ≈8 КБ/с: 12 КБ прошивки заливаются примерно за 1.5 с. Для обновления
  «раз в год» этого более чем достаточно; окно >1 потребовало бы приёмного
  буфера на несколько кадров и отдельного разбора «что уже записано»;
* проверка чтением читает образ целиком: 12 КБ на 72 МГц — единицы
  миллисекунд, но при желании её можно выключить (`verify_readback = false`),
  понимая, что тогда «Ok от носителя» принимается на веру;
* стирание слота идёт постранично и с окном сторожу, поэтому слот стирается не
  мгновенно (28 КБ на STM32F1 — примерно секунда); метка состояния снимается
  ДО стирания, чтобы сбой питания посреди операции не оставил слот помеченным
  «годным», а пустым;
* переход на новую прошивку выполняет загрузчик (см. следующий раздел): модуль
  даёт ему всё, что нужно для решения, — состояние слота, версию,
  CRC32/SHA-256 и счётчик попыток загрузки в записи слота и в резервных
  регистрах.

## Загрузчик: переход на новую прошивку

Подсистема OTA умеет принять образ, проверить его и оставить метки («слот B
активирован, попыток загрузки — 1»). Передать управление она не умеет и не
должна: стирание страницы, из которой исполняется код, и перенос таблицы
векторов — работа того, кто уже не зависит от загружаемой прошивки. Это и есть
загрузчик: модуль `os/boot/`, образ `bootloader/bootloader_fw`.

### Что делает загрузчик

1. **Решает, что грузить.** Читает метки (общий с os/ota `SlotStore`), выбирает
   слот: активированный хостом важнее подтверждённого, подтверждённый — просто
   проверенного.
2. **Проверяет образ ПЕРЕД переходом** — правдоподобие вектора сброса и, по
   настройке, CRC32 и SHA-256 по носителю. Метка говорит лишь, что образ
   когда-то был проверен; между «проверено» и «загружаем» могло пройти сколько
   угодно времени и сбоев питания, а прыжок в испорченный образ — это отказ без
   диагностики и без возможности обновиться.
3. **Считает пробные загрузки.** Активированный слот грузится как пробный:
   счётчик увеличивается, и если прошивка не подтвердила себя
   (`ota::Service::confirm_running()` из приложения), загрузчик **откатывается**
   на прежний слот. Это защита от «кирпича»: падающая на старте прошивка не
   может перезагружаться вечно.
4. **Передаёт управление** (VTOR, MSP, обработчик сброса) — это контракт
   `platform::BootOps`, то есть платформа. На стенде переход моделируется, и его
   параметры проверяются тестами без платы.
5. **Опционально принимает прошивку сам** — если собран с
   `-DMCFRAME_BOOTLOADER_OTA=ON` (см. раздел про размеры ниже).

### Раскладка Flash

Схема «загрузчик + два слота» для STM32F103RB (128 КБ). Числа задаются в ОДНОМ
месте (`cmake/firmware_layout.cmake`) и уходят и в линкер (`--defsym`), и в C++
(`os/boot/boot_layout.hpp`), поэтому загрузчик, приложение и линкер не могут
разойтись в адресах:

```
0x08000000 ┌────────────────────────────┐
           │ загрузчик (bootloader_fw)  │  20 КБ  (занято ≈19.05 КБ)
0x08005000 ├────────────────────────────┤
           │ слот A (example_fw)        │  52 КБ  (занято ≈51 КБ)
0x08012000 ├────────────────────────────┤
           │ слот B (новая прошивка)    │  52 КБ
0x0801F000 ├────────────────────────────┤
           │ метаданные OTA             │  4 КБ  (две копии записи на слот)
0x08020000 └────────────────────────────┘  128 КБ — вся Flash
```

Раскладка занимает всю Flash: запас есть ВНУТРИ областей, а не «между» ними, —
так правка в загрузчике не требует перекраивать адреса слотов. Другие размеры:

```bash
# меньше слотов — больше загрузчику, и наоборот:
./build.sh stm32 Release -DMCFRAME_FLASH_BOOT_SIZE=20K -DMCFRAME_FLASH_SLOT_SIZE=52K
# плата с другим размером Flash (раскладка обязана в него влезать):
./build.sh stm32 Release -DMCFRAME_FLASH_TOTAL=256K \
    -DMCFRAME_FLASH_BOOT_SIZE=48K -DMCFRAME_FLASH_SLOT_SIZE=100K
```

**Размеры измерены, а не взяты «на глаз»** (Release, LTO):

| Что | Размер | Комментарий |
|---|---|---|
| Загрузчик без приёма OTA | ≈19.05 КБ | Отведено 20 КБ (95 %) |
| Загрузчик С приёмом OTA | ≈48 КБ | Тогда слоты придётся уменьшить до ≈38 КБ: на 128 КБ Flash выбирать приходится осознанно |
| «Минимальное» приложение примера | ≈51 КБ | Влезает в слот 52 КБ (демонстрации выключены: `-DMCFRAME_APP_MINIMAL=ON`) |
| Полная «витрина» примера | ≈67 КБ | В слот 52 КБ НЕ влезает — это и есть та причина, по которой демонстрации выключаются автоматически при `-DMCFRAME_IMAGE=slot` |

Именно из этих чисел и получилась раскладка: 20 + 52 + 52 + 4 = 128 КБ. На
плате с 256 КБ (STM32F103RC) приём можно оставить и в загрузчике, и в
приложении сразу.

Если приложение не влезает в слот, это **ошибка линковки**, а не «странное
поведение на устройстве» — ровно то, что нужно.

### Сборка и первая прошивка

```bash
./build.sh stm32 Release -DMCFRAME_IMAGE=slot      # приложение в слоте A
```

Собираются два образа: `bootloader_fw` (с 0x08000000) и `example_fw` (по адресу
слота A). Оба — `.hex` и `.bin`. Первый раз их кладут программатором:

```bash
st-flash write .build/stm32/bootloader_fw.bin 0x08000000
st-flash write .build/stm32/example_fw.bin    0x08005000   # слот A
```

Дальше образ заливается уже по OTA: принимает его приложение (в нём тот же
сервис OTA), а загрузчик грузит активированный слот, считает попытки и умеет
откатиться.

**Таблицу векторов приложение переставляет САМО.** После сброса VTOR указывает
на загрузчик (или на начало Flash), а `SystemInit` живёт в библиотеке и собран
ОДИН раз для всех образов — передать ему «смещение на образ» через `-D` нельзя.
Поэтому каждая прошивка в начале `main` вызывает
`boot::relocate_vectors_to_self()`: адрес берётся из символа линкера
`_os_image_base`, и первое же прерывание после этого попадает в СВОЮ таблицу.
Забудете вызвать — тик и приём байта уйдут в чужой код, и искать это будут в
драйверах (в собранном образе видно, что вызов на месте: `relocate_vectors`
вызывается с адресом слота).

**Прошивка без метаданных.** У только что залитого программатором образа метки
нет, и загрузчик обязан что-то с этим делать: иначе устройство осталось бы в
загрузчике навсегда. Поэтому «похожий на образ» слот допускается к загрузке
(`Config::allow_unrecorded_image`, по умолчанию включено), но со счётчиком
попыток в резервных регистрах (у STM32 — BKP, они переживают сброс): не
подтвердив себя несколько раз, образ помечается негодным и больше не грузится.
Хотите «только то, что принято по OTA» — выключите эту настройку.

### Поток обновления целиком

```
хост                          загрузчик                  приложение
 │                               │                          │
 │                          сброс/включение                 │
 │                               ├─ boot::evaluate()        │
 │                               │   метки + проверка образа│
 │   (окно 500 мс, если загрузчик умеет принимать)          │
 │                               ├─────────────────────────►│ переход (VTOR/MSP)
 │                               │                          ├─ boot_check()
 │                               │                          │   «эта загрузка пробная»
 │                               │                          ├─ confirm_self()
 │                               │                          │   счётчик попыток = 0
 │  INFO/START/DATA/END ────────►│                          │
 │  (в приложение или, если     │                          │
 │   OTA в загрузчике, в него)  │                          │
 │  ACTIVATE(slot) ─────────────►│                          │
 │                               │                    (перезагрузка)
 │                          сброс/перезагрузка             │
 │                               ├─ грузит новый слот (пробно)
 │                               │  не подтвердился N раз → откат на прежний
```

### Что проверено, а что нет

Тесты (`tests/test_boot.cpp`, 96 проверок) прогоняют на модели носителя:
выбор слота, параметры перехода (адрес, VTOR, запрет прерываний), отказ при
неправдоподобном векторе и при несошедшейся CRC32/SHA-256, счётчик пробных
загрузок и откат, «откатываться некуда», окно ожидания хоста, образ без метки и
подтверждение загрузки со стороны приложения. «Прыжок в мусор» проверяется на
РЕАЛЬНЫХ байтах вектора в модели Flash, а не на флаге в структуре.

Чего тесты не делают: они не заменяют проверку на железе. Сам переход
(`platforms/stm32/boot_stm32.cpp`: запрет прерываний, остановка SysTick, сброс
`NVIC->ICPR`, `VTOR`, `__set_MSP`, `bx` на обработчик сброса) собирается под ARM
и должен быть проверен на плате — на стенде он именно моделируется.


## Непрерывная интеграция

`.github/workflows/ci.yml` проверяет то, что легко сломать незаметно:

* **mock Debug** — сборка с ASan/UBSan и все 14 наборов;
* **mock Release** — LTO-сборка (проверяет, что LTO не ломает таблицы
  векторов/`naked`-функции) и те же наборы;
* **stm32** — кросс-сборка `flat`, `flat + shell` и `slot`, плюс бюджет
  размера: `tools/check_size.sh` требует, чтобы `bootloader_fw.bin` был
  ≤ 20 КБ, `example_fw.bin` в flat — ≤ 128 КБ, в slot — ≤ 52 КБ. «Собралось,
  но не влезло» — ошибка CI, а не сюрприз на плате.

Скрипт можно запускать локально:

```bash
tools/check_size.sh .build/stm32/bootloader_fw.bin 20480 "bootloader"
tools/check_size.sh .build/stm32/example_fw.bin 53248 "slot"
```

## Требования

* CMake ≥ 3.20
* mock: GCC/Clang (проверено на GCC 16)
* stm32: `arm-none-eabi-gcc` + `arm-none-eabi-gcc-ar` (для сборок с LTO — то
  есть для `Release`; без `gcc-ar` собирайте с `-DMCFRAME_LTO=OFF`)

## Тесты

```bash
./build.sh mock
ctest --test-dir .build/mock --output-on-failure
# или напрямую:
.build/mock/tests/mc_tests           # ядро: 221 проверка
.build/mock/tests/mc_tests_async     # async IO: 54 проверки
.build/mock/tests/mc_tests_periph    # UART/I2C на mock: 192 проверки
.build/mock/tests/mc_tests_stm32hal  # HAL-логика STM32: 257 проверок
.build/mock/tests/mc_tests_crypto    # криптография: 187 проверок
.build/mock/tests/mc_tests_dma_arbiter  # арбитр каналов: 80 проверок
.build/mock/tests/mc_tests_power     # супервизор энергосбережения: 103 проверки
.build/mock/tests/mc_tests_stm32power   # STOP/RTC/частота на модели: 48 проверок
.build/mock/tests/mc_tests_watchdog  # сторожевой таймер: 96 проверок
.build/mock/tests/mc_tests_stm32watchdog  # IWDG/BKP на модели: 44 проверки
.build/mock/tests/mc_tests_ota       # OTA: протокол, слоты, выравнивание, продолжение: 290 проверок
.build/mock/tests/mc_tests_boot      # загрузчик: выбор слота, проверка образа, переход: 96 проверок
.build/mock/tests/mc_tests_fault     # P0: запись об аварии, wait queue, PI, таймаут, стек: 76 проверок
.build/mock/tests/mc_tests_ipc       # P1: очереди/события/WorkQueue/log/shell/статистика: 98 проверок
```

`ctest` прогоняет четырнадцать наборов: `core_selftest`, `async_io`, `uart_i2c`,
`stm32_hal_strategies`, `dma_arbiter`, `power_supervisor`, `stm32_power`,
`watchdog`, `stm32_watchdog`, `boot`, `ota`, `crypto`, `fault_and_wait` и
`ipc_shell`.

Набор `dma_arbiter` (`tests/host/test_dma_arbiter.cpp`) проверяет собственно
арбитрацию, без драйверов и железа: ожидание занятого канала (USART1_TX и
SPI2_RX — один DMA1 Ch4), параллельность разных каналов, «одна работа на
интерфейс», FIFO, захват пары каналов разом, завершение прямо внутри `start()`,
ошибка старта, таймаут и отмена, переполнение очереди. Интеграция с настоящими
драйверами (очередь из трёх запросов UART1 к занятому каналу, параллельная
работа UART1 + I2C1) — в наборе `stm32_hal_strategies`.

### HAL-логика STM32 на ПК (`stm32_hal_strategies`)

Автоматы `uart_stm32.hpp`/`i2c_stm32.hpp` — это тот код, который уходит в
прошивку, поэтому проверять их только через mock-драйверы нельзя: mock
повторяет идею, а не реализацию. Набор `tests/host/` собирает НАСТОЯЩИЕ
заголовки STM32-драйверов на ПК, подменив MMIO на программную модель
регистров:

* `tests/host/stm32_regs_fake.hpp` — модель USART1/I2C1/RCC/NVIC/PWR/RTC/EXTI/
  FLASH/SCB/SysTick. Битовые маски, имена регистров и номера IRQ берутся из
  настоящей CMSIS, а чтение и запись регистров идут через хуки: модель видит
  `DR`, снимает `RXNE` при чтении, ставит `ADDR` при ответе устройства,
  выставляет `HSIRDY/PLLRDY/LSERDY` после включения источника, переключает
  `SWS` вслед за `SW`, а `__WFI()` заменяется вызовом модели (на хосте нет
  ARM-ассемблера), поэтому тест проверяет ФАКТ остановки ядра;
* `tests/host/fake_dma.cpp` — модель DMA1, реализующая тот же контракт
  `platform_dma_*`, что и `platforms/stm32/platform.cpp`: канал, `CPAR`,
  `CNDTR`, флаги `TC/TE`, колбэк с `DmaCallbackContext`;
* переключатель подмены — один `#if` в `platforms/stm32/stm32_regs.hpp`;
  в самих драйверах ветвлений «для теста» нет.

Что проверяется: завершение передачи по `TC` (а не по записи в `DR`), приём
по `IDLE` при буфере больше пакета, выключенные прерывания и NVIC в режиме
`Polling`, обязательные `DMAT/DMAR`/`DMAEN` в режиме `Dma`, `N-1` байт через
DMA и последний байт программно у I2C, `NotFound` на NACK адреса, ошибки
канала DMA.

Чего этот набор НЕ делает: он не заменяет проверку на железе. Модель
воспроизводит только то поведение регистров, на которое опираются драйверы.

Набор тестов (`tests/test_os.cpp`) фиксирует каждый исправленный дефект ядра:
арифметику `BigInt` (деление, переносы, алиасинг), бухгалтерию кучи
(статистика, двойной `free`, фрагментация), работу `add_task`/`remove_task`,
учётную середину переключения контекста (`prepare_context_switch`: возвращается
SP именно той задачи, которая стала текущей) и корректность утилит.

Набор `tests/test_crypto.cpp` построен на **стандартных тестовых векторах**
(FIPS 180-4/197/202, NIST SP 800-38A, RFC 4231, SEC 2/FIPS 186-4, каталог CRC)
и на проверках каждого исправленного дефекта модуля: константы SHA-2, усечение
дайджеста, расшифрование AES-CTR без таблицы обратных подстановок, размер
блока TDES, переполнение модуля RSA, точки кривых ECC, обнаружение подмены в
ECIES.

### Энергосбережение на ПК (`power_supervisor`, `stm32_power`)

Два набора проверяют то, цена ошибки в чём — не «медленно», а «уснул и не
проснулся»:

* `power_supervisor` (`tests/test_power.cpp`) — ПОЛИТИКА на модели
  энергосбережения mock-платформы (`src/platforms/mock/platform_power_mock.hpp`):
  учёт нагрузки по окнам (60 % и 80 % на разных окнах, средняя, пик), горизонт
  сна по спящим задачам и таймерам ОС, отказы от глубокого сна (нет источника,
  короткий горизонт, взятое удержание, отказ платформы), продвижение времени ОС
  и «догон» задержек задач после сна. Отдельно проверяется проекция нагрузки на
  другую частоту — арифметика, по которой выбирается статический профиль.
* `stm32_power` (`tests/host/test_stm32_power.cpp`) — ПОСЛЕДОВАТЕЛЬНОСТЬ работы
  с регистрами: собирается настоящий `platforms/stm32/power_stm32.cpp` с
  `-DMCFRAME_STM32_DEEP_SLEEP=1` и моделью RTC/PWR/EXTI/FLASH/SCB/SysTick.
  Проверяется, что будильник запрограммирован (и только в режиме конфигурации
  RTC), линия EXTI17 и прерывание разрешены до остановки ядра, SysTick в момент
  `WFI` не идёт, `SLEEPDEEP` снимается после выхода, время сна ИЗМЕРЕНО по RTC,
  тактирование и тик возвращены, а смена профиля частоты сопровождается
  переключением источника и wait states Flash.

Модель намеренно «вредная»: если будильник не вооружён, `WFI` оставляет ядро в
STOP (`CoreModel::stuck_in_stop`), а запись в регистры RTC вне режима
конфигурации игнорируется — как на железе. Иначе тест «проходил» бы и на коде,
который на плате не проснулся бы никогда.

Чего эти наборы НЕ делают: они не измеряют ток и не заменяют проверку на
плате. Модель воспроизводит только ту логику регистров, на которую опирается
код энергосбережения.

### Сторожевой таймер на ПК (`watchdog`, `stm32_watchdog`)

* `watchdog` (`tests/test_watchdog.cpp`) — ПОЛИТИКА на модели
  (`src/platforms/mock/platform_watchdog_mock.hpp`). Модель не перезагружает
  хост, а считает, сколько раз железо сработало бы, поэтому проверяется именно
  решение сервиса: кормление по расписанию, обнаружение просрочки по сроку и по
  явному `fail()`, ПОСМЕРТНАЯ ЗАПИСЬ (кто и на сколько опоздал, номер
  загрузки, отсечение устаревшей отметки), защёлка (оживший участник не
  отменяет сброс), `rearm`, ранняя отметка вместо окна WWDG, отказы в
  длительных операциях и — главное — согласование с глубоким сном: сон не
  длиннее срока сторожа, а после пробуждения сроки участников начинаются заново.
* `stm32_watchdog` (`tests/host/test_stm32_watchdog.cpp`) — РЕГИСТРЫ:
  собирается настоящий `platforms/stm32/watchdog_stm32.cpp` с моделью IWDG/BKP/
  RCC_CSR. Проверяется, что PR/RLR записаны только после ключа `0x5555`,
  флаги `PVU/RVU` ДОЖДАЛИСЬ снятия, счётчик перезагружен ключом `0xAAAA`,
  а расчёт таймаута даёт гарантированный минимум на всём разбросе LSI
  (30–60 кГц). Модель намеренно «вредная»: запись без ключа теряется, поэтому
  тест не может пройти на коде, забывшем разрешить запись.

### Аварии и ожидание (`fault_and_wait`)

`tests/test_fault.cpp` проверяет P0-механизмы на синтетическом кадре
исключения и на учётной части планировщика:

* запись об аварии: magic/kind/pc/lr/cfsr/line/task, CRC, basename файла,
  `was_fault_reset()`, `clear()`, `report()`; BKP-метка пишется и читается;
* `wait_queue`: порядок по приоритету и FIFO внутри приоритета, повторная
  постановка, `remove`, `pop_highest`;
* `mutex`: наследование приоритета владельцем и восстановление после
  `unlock`, отдельное изменение действующего приоритета;
* таймаут: `block_on(3)` → `advance_ticks(3)` → `timed_out` и готовность;
* `wake_pending`: пробуждение, пришедшее до `block_on`, не теряется;
* сторожевой маркер стека: целый/затёртый/восстановленный.

На mock нет реального переключения контекста, поэтому проверяется учётная
часть — именно она на железе решает, кого разбудят и с каким приоритетом.

### IPC, shell, статистика (`ipc_shell`)

`tests/test_ipc.cpp` проверяет P1-механизмы на учётной части планировщика:

* `MessageQueue`: push/pop, заполнение/опустошение, `try_*`, ISR-варианты,
  тип без default-конструктора;
* **блокирующие** `MessageQueue::pop`, `EventFlags::wait`, `Mailbox::pend` —
  через тестовый хук планировщика, который сигналит ровно в момент постановки
  задачи в очередь (закрывает окно потерянного пробуждения) и через
  `advance_ticks()` проверяет абсолютный таймаут;
* `Mailbox`: перезапись последнего значения, `pend/try_pend/peek/clear`;
* `EventFlags`: `any/all`, автоочистка, `set_from_isr`, `wait(0)`;
* `WorkQueue`: постановка до `start`, запуск задачи, `run_one/drain`,
  переполнение;
* `DeferredTimer`: создание/удаление таймера поверх WorkQueue;
* `log`: уровни, кольцо, `from_isr`, фильтрация, `pop`;
* shell: `help/ps/free/stats/log`, неизвестная команда, ввод через `feed`,
  опциональные `wd`/`ota`;
* статистика и round-robin: `runtime_ticks`, `switches`, снимок задач,
  переключение равноприоритетных задач по кванту.

mock-сборка компилируется с `-Wall -Wextra -Wconversion -Wshadow` и
запускается под ASan/UBSan (`-DMCFRAME_SANITIZE=ON`, включено по умолчанию) —
это ловит порчу памяти и UB, которые на МКУ проявляются как HardFault.
Тесты выполняются только для `mock`: они используют платформенные заглушки.
