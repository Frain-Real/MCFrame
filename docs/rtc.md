# Tickless idle и RTC wall clock

## Tickless idle

MCFrame уже не «тикает вхолостую» на STM32: `power::Supervisor` в idle-задаче
останавливает ядро (`WFI`) или уходит в STOP, предварительно вооружив RTC-
будильник и остановив SysTick. После выхода платформа возвращает ИЗМЕРЕННОЕ
время, а супервизор продвигает `OsTimer` и спящие задачи:

```cpp
power::Config cfg;
cfg.allow_deep_sleep  = true;
cfg.deep_sleep_min_ms = 8u;       // окупаемость входа/выхода
cfg.deep_sleep_max_ms = 60000u;   // верхняя граница одного сна
power::Supervisor::instance().init(cfg);
```

Что проверяется в [`tests/test_power.cpp`](../tests/test_power.cpp):

- нет источника пробуждения → DeepSleep запрещён;
- будильник грубый (1000 мс) → короткий сон не окупается;
- `keep_awake()` запрещает STOP;
- платформа вернула меньше/больше запрошенного → время ОС двигается на
  фактическое;
- сторожевой таймер ограничивает длину сна.

## RTC wall clock

[`src/os/time/rtc.hpp`](../src/os/time/rtc.hpp) — календарное время,
переживающее сброс:

```cpp
rtc::WallClock& clk = rtc::WallClock::instance();
rtc::SoftBackend backend;              // на стенде; на F1 — stm32::rtc::backend()
clk.attach(&backend);

// После загрузки: взять время из RTC и применить к OsTimer.
// Если RTC пуст, но ОС уже знает время — записать его в RTC.
(void)clk.init();

// Время из SNTP/ручного ввода:
(void)clk.set_epoch(1767225600ull);    // 2026-01-01T00:00:00Z

DateTime now = clk.now();
```

`Backend` — единственное, что знает про железо:

| платформа | backend |
|---|---|
| STM32F1 | `stm32::rtc::backend()`, BKP magic + RTC CNT (секунды Unix) |
| STM32F4/G4 | `stm32f4g4::rtc::backend()`, RTC TR/DR BCD + BKP0R |
| mock | `rtc::SoftBackend` |

`WallClock::set_epoch()` вызывает `OsTimer::SyncTime()` и `persist()`, поэтому
календарные будильники (`OsTimer::SetAlarm`) и SNTP используют одно время.

## Порядок инициализации

1. `stm32::power::init()` — выбирает LSE/LSI и запускает RTC (если включён
   deep sleep), либо board-backend.
2. `OsTimer::Init()`.
3. `rtc::WallClock::instance().init()` — синхронизирует `OsTimer` из RTC.
4. `OsTimer::Start()`.

`board::init()` на F1/F4/G4 привязывает соответствующий backend. RTC стартует
лениво при первом `read/write`: LSE, при отказе — LSI. Если время ещё не
записано, `init()` вернёт `TimeNotSynced` — это нормально для новой платы без
батарейки.

## Ограничения

- F1 RTC хранит 32-битные секунды: диапазон 1970–2106.
- F4/G4 RTC поддерживает 2000–2099 (двузначный год в BCD).
- `SoftBackend` — RAM-only; он нужен для host-тестов и плат без RTC.
- F4/G4 требуют LSE/LSI и backup-домен; при отказе LSE используется LSI
  (точность хуже).
