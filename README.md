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
