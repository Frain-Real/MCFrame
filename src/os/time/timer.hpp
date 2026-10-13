#pragma once

#include "../common.hpp"
namespace embed {

// ----------------------------------------------------------------------------
// Base helpers
// ----------------------------------------------------------------------------

constexpr uint32_t Gcd(uint32_t a, uint32_t b) noexcept
{
    while (b != 0) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

// ----------------------------------------------------------------------------
// Calendar date/time
// ----------------------------------------------------------------------------

struct DateTime
{
    uint16_t year;
    uint8_t  month;   // 1..12
    uint8_t  day;     // 1..31
    uint8_t  hour;    // 0..23
    uint8_t  minute;  // 0..59
    uint8_t  second;  // 0..59
};

constexpr bool IsLeapYear(uint16_t year) noexcept
{
    return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

constexpr uint8_t DaysInMonth(uint16_t year, uint8_t month) noexcept
{
    switch (month) {
        case 1:  return 31;
        case 2:  return IsLeapYear(year) ? 29 : 28;
        case 3:  return 31;
        case 4:  return 30;
        case 5:  return 31;
        case 6:  return 30;
        case 7:  return 31;
        case 8:  return 31;
        case 9:  return 30;
        case 10: return 31;
        case 11: return 30;
        case 12: return 31;
        default: return 0;
    }
}

constexpr bool IsValid(const DateTime& dt) noexcept
{
    if (dt.month < 1 || dt.month > 12) {
        return false;
    }

    uint8_t dim = DaysInMonth(dt.year, dt.month);
    if (dt.day < 1 || dt.day > dim) {
        return false;
    }

    if (dt.hour > 23 || dt.minute > 59 || dt.second > 59) {
        return false;
    }

    return true;
}

// Civil date -> days since Unix epoch.
// Алгоритм в стиле Howard Hinnant, без libc.
constexpr int64_t DaysFromCivil(int y, int m, int d) noexcept
{
    y -= (m <= 2) ? 1 : 0;

    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);

    const unsigned mp  = static_cast<unsigned>(m + (m > 2 ? -3 : 9)); // 0..11
    const unsigned doy = (153 * mp + 2) / 5 + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

constexpr uint64_t ToEpochSeconds(const DateTime& dt) noexcept
{
    int64_t days = DaysFromCivil(static_cast<int>(dt.year),
                                 static_cast<int>(dt.month),
                                 static_cast<int>(dt.day));

    int64_t s = days * 86400 +
                static_cast<int64_t>(dt.hour) * 3600 +
                static_cast<int64_t>(dt.minute) * 60 +
                static_cast<int64_t>(dt.second);

    return (s > 0) ? static_cast<uint64_t>(s) : 0;
}

constexpr DateTime FromEpochSeconds(uint64_t epoch_sec) noexcept
{
    int64_t s = static_cast<int64_t>(epoch_sec);
    int64_t days = s / 86400;
    int rem = static_cast<int>(s % 86400);

    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }

    int hour   = rem / 3600;
    rem %= 3600;
    int minute = rem / 60;
    int second = rem % 60;

    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;

    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;

    int64_t y64 = static_cast<int64_t>(yoe) + era * 400;

    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp  = (5 * doy + 2) / 153;

    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    int m = static_cast<int>(mp) + (mp < 10 ? 3 : -9);

    y64 += (m <= 2) ? 1 : 0;

    if (y64 < 1970 || y64 > 9999) {
        return DateTime{1970, 1, 1, 0, 0, 0};
    }

    return DateTime{
        static_cast<uint16_t>(y64),
        static_cast<uint8_t>(m),
        static_cast<uint8_t>(d),
        static_cast<uint8_t>(hour),
        static_cast<uint8_t>(minute),
        static_cast<uint8_t>(second)
    };
}

// ----------------------------------------------------------------------------
// Timer module
// ----------------------------------------------------------------------------

enum class Repeat : uint8_t
{
    Once = 0,
    Daily,
    Weekly
};

// Platform должна предоставить:

 void platform_timer_init() noexcept;
 bool platform_timer_can_period_ms(uint32_t period_ms) noexcept;
 void platform_timer_set_period_ms(uint32_t period_ms) noexcept;
 void platform_timer_start() noexcept;
 void platform_timer_stop() noexcept;
 void platform_enter_critical() noexcept;
 void platform_exit_critical() noexcept;

// TimerSetPeriodMs() должен программировать аппаратный таймер так,
// чтобы его прерывание вызывалось каждые period_ms.

template <
    uint8_t MaxTimers,
    uint8_t MaxAlarms,
    uint8_t MaxSystemHooks = 4
>
class TimerModule
{
public:
    struct TimerHandle
    {
        uint8_t  index = 0xFF;
        uint16_t gen   = 0;
    };
    struct AlarmHandle
    {
        uint8_t  index = 0xFF;
        uint16_t gen   = 0;
    };
    struct Config
    {
        // Минимальный период аппаратного тика.
        // Желательно 1 мс, если железо позволяет.
        uint32_t min_tick_ms = 1;

        // Максимальный период аппаратного тика.
        // Обычно 1000 мс достаточно для календарных будильников.
        uint32_t max_tick_ms = 1000;

        // Тик по умолчанию, если нет активных таймеров/будильников.
        uint32_t default_tick_ms = 10;

        // Период проверки календарных будильников.
        // Если есть активный будильник, модуль попытается сделать тик
        // кратным этому значению.
        uint32_t alarm_tick_ms = 1000;

        // Максимальное число "догоняющих" вызовов периодического таймера
        // за один тик, если прерывания были задержаны.
        uint8_t max_catchup = 3;

        // Если true, календарное время считается валидным сразу.
        // Обычно нужно только для тестов.
        bool start_time_valid = false;
    };

    // Init / start
    static Status Init(const Config& cfg) noexcept
    {
        Lock lock;
        cfg_ = cfg;
        if (cfg_.min_tick_ms == 0) {
            cfg_.min_tick_ms = 1;
        }
        if (cfg_.max_tick_ms < cfg_.min_tick_ms) {
            return Status::InvalidArg;
        }
        if (cfg_.default_tick_ms == 0) {
            cfg_.default_tick_ms = cfg_.min_tick_ms;
        }
        if (cfg_.alarm_tick_ms == 0) {
            cfg_.alarm_tick_ms = cfg_.max_tick_ms;
        }
        if (cfg_.max_catchup == 0) {
            cfg_.max_catchup = 1;
        }
        if (cfg_.default_tick_ms < cfg_.min_tick_ms) {
            cfg_.default_tick_ms = cfg_.min_tick_ms;
        }
        if (cfg_.default_tick_ms > cfg_.max_tick_ms) {
            cfg_.default_tick_ms = cfg_.max_tick_ms;
        }
        now_ms_ = 0;
        wall_offset_ms_ = 0;
        tick_ms_ = cfg_.default_tick_ms;
        time_valid_ = cfg_.start_time_valid;
        running_ = false;
        for (Size i = 0; i < kTimerStorage; ++i) {
            timers_[i] = TimerSlot{};
        }
        for (Size i = 0; i < kAlarmStorage; ++i) {
            alarms_[i] = AlarmSlot{};
        }
        for (Size i = 0; i < kHookStorage; ++i) {
            hooks_[i] = HookSlot{};
        }
        platform_timer_init();

        return Status::Ok;
    }

    static Status Start() noexcept
    {
        Lock lock;

        if (running_) {
            return Status::Ok;
        }

        uint32_t desired = ComputeDesiredTickLocked();
        Status st = ApplyTickLocked(desired);

        if (st != Status::Ok) {
            return st;
        }

        platform_timer_start();
        running_ = true;

        return Status::Ok;
    }

    static Status Stop() noexcept
    {
        Lock lock;

        if (!running_) {
            return Status::Ok;
        }

        platform_timer_stop();
        running_ = false;

        return Status::Ok;
    }

    // Hardware ISR entry

    // Вызывать из обработчика прерывания аппаратного таймера.
    // Например:
    //
    // extern "C" void TIM6_IRQHandler()
    // {
    //     OsTimer::TickIsr();
    // }
    //
    static void TickIsr() noexcept
    {
        uint64_t now = now_ms_ + tick_ms_;
        now_ms_ = now;

        ProcessTimers(now);
        ProcessAlarms(now);
        // Системный тик: сюда подключаются периодические системные функции,
        // например Scheduler::Tick().
        SystemTick();
    }

    // Функция системного тика.
    // Если у вас уже есть Scheduler::Tick(), зарегистрируйте его через
    // RegisterSystemHook(), либо вручную добавьте вызов сюда.
    static void SystemTick() noexcept
    {
        RunSystemHooks();
    }

    // System hooks

    // Регистрация периодической системной функции, которая будет вызываться
    // каждый системный тик.
    //
    // Пример подключения планировщика:
    //
    // void SchedulerTickHook(void*) { Scheduler::Tick(); }
    // OsTimer::RegisterSystemHook(SchedulerTickHook, nullptr);
    //
    static Status RegisterSystemHook(Callback_h cb, void* arg) noexcept
    {
        if (cb == nullptr) {
            return Status::InvalidArg;
        }

        Lock lock;
        for (uint8_t i = 0; i < MaxSystemHooks; ++i) {
            if (!hooks_[i].used) {
                hooks_[i].used = true;
                hooks_[i].cb = cb;
                hooks_[i].arg = arg;
                return Status::Ok;
            }
        }
        return Status::Aborted;
    }

    // Software timers
    static Status CreateTimer(uint32_t period_ms,
                              bool periodic,
                              Callback_h cb,
                              void* arg,
                              TimerHandle& out) noexcept
    {
        if (period_ms == 0 || cb == nullptr) {
            return Status::InvalidArg;
        }

        Lock lock;
        for (uint8_t i = 0; i < MaxTimers; ++i) {
            TimerSlot& s = timers_[i];
            if (!s.used) {
                s.used = true;
                s.active = false;
                s.periodic = periodic;
                s.period_ms = period_ms;
                s.due_ms = 0;
                s.cb = cb;
                s.arg = arg;
                out.index = i;
                out.gen = s.gen;
                return Status::Ok;
            }
        }
        return Status::Aborted;
    }

    static Status StartTimer(TimerHandle h) noexcept
    {
        Lock lock;
        if (!TimerValidLocked(h)) {
            return Status::InvalidArg;
        }

        TimerSlot& s = timers_[h.index];
        if (s.active) {
            return Status::Ok;
        }
        if (s.period_ms == 0) {
            return Status::InvalidArg;
        }
        s.due_ms = now_ms_ + s.period_ms;
        s.active = true;
        s.seq++;
        Status st = UpdateTickLocked();
        if (st != Status::Ok) {
            // Откат.
            s.active = false;
            s.seq++;
            return st;
        }
        return Status::Ok;
    }

    static Status StopTimer(TimerHandle h) noexcept
    {
        Lock lock;
        if (!TimerValidLocked(h)) {
            return Status::InvalidArg;
        }
        TimerSlot& s = timers_[h.index];
        if (!s.active) {
            return Status::Ok;
        }
        s.active = false;
        s.seq++;
        UpdateTickLocked();
        return Status::Ok;
    }

    static Status SetTimerPeriod(TimerHandle h, uint32_t period_ms) noexcept
    {
        if (period_ms == 0) {
            return Status::InvalidArg;
        }
        Lock lock;
        if (!TimerValidLocked(h)) {
            return Status::InvalidArg;
        }
        TimerSlot& s = timers_[h.index];
        uint32_t old_period = s.period_ms;
        uint64_t old_due = s.due_ms;
        uint16_t old_seq = s.seq;
        s.period_ms = period_ms;
        if (s.active) {
            s.due_ms = now_ms_ + period_ms;
        }
        s.seq++;
        Status st = UpdateTickLocked();
        if (st != Status::Ok) {
            s.period_ms = old_period;
            s.due_ms = old_due;
            s.seq = static_cast<uint16_t>(old_seq + 1);
            return st;
        }
        return Status::Ok;
    }

    static Status RemoveTimer(TimerHandle h) noexcept
    {
        Lock lock;
        if (!TimerValidLocked(h)) {
            return Status::InvalidArg;
        }
        TimerSlot& s = timers_[h.index];
        s.used = false;
        s.active = false;
        s.periodic = false;
        s.cb = nullptr;
        s.arg = nullptr;
        s.gen++;
        s.seq++;
        UpdateTickLocked();
        return Status::Ok;
    }

    // Calendar alarms

    // Устанавливает календарный будильник.
    // Требует синхронизированного времени: сначала вызвать SyncTime().
    static Status SetAlarm(const DateTime& at,
                           Repeat repeat,
                           Callback_h cb,
                           void* arg,
                           AlarmHandle& out) noexcept
    {
        if (!IsValid(at) || cb == nullptr) {
            return Status::InvalidArg;
        }

        Lock lock;
        if (!time_valid_) {
            return Status::TimeNotSynced;
        }
        for (uint8_t i = 0; i < MaxAlarms; ++i) {
            AlarmSlot& a = alarms_[i];
            if (!a.used) {
                a.used = true;
                a.active = true;
                a.repeat = repeat;
                a.due_sec = ToEpochSeconds(at);
                a.cb = cb;
                a.arg = arg;

                out.index = i;
                out.gen = a.gen;
                UpdateTickLocked();
                return Status::Ok;
            }
        }
        return Status::Aborted;
    }

    static Status CancelAlarm(AlarmHandle h) noexcept
    {
        Lock lock;
        if (!AlarmValidLocked(h)) {
            return Status::InvalidArg;
        }

        AlarmSlot& a = alarms_[h.index];
        a.used = false;
        a.active = false;
        a.cb = nullptr;
        a.arg = nullptr;
        a.gen++;
        a.seq++;
        UpdateTickLocked();
        return Status::Ok;
    }

    // Time synchronization

    // Синхронизация календарного времени.
    //
    // Например, после получения времени из RTC или по сети:
    //
    // OsTimer::SyncTime(1735689600ULL, 0);
    //
    static Status SyncTime(uint64_t epoch_sec, uint32_t ms = 0) noexcept
    {
        if (ms >= 1000) {
            return Status::InvalidArg;
        }
        if (epoch_sec > kMaxEpochSec) {
            return Status::InvalidArg;
        }

        Lock lock;
        int64_t target_ms = static_cast<int64_t>(epoch_sec) * 1000 +
                            static_cast<int64_t>(ms);
        wall_offset_ms_ = target_ms - static_cast<int64_t>(now_ms_);
        time_valid_ = true;
        return Status::Ok;
    }

    // Плавная/ручная корректировка календарного времени.
    // Положительное значение двигает время вперёд, отрицательное назад.
    static Status AdjustTimeMs(int32_t delta_ms) noexcept
    {
        Lock lock;
        if (!time_valid_) {
            return Status::TimeNotSynced;
        }
        wall_offset_ms_ += static_cast<int64_t>(delta_ms);
        return Status::Ok;
    }

    static bool TimeValid() noexcept
    {
        Lock lock;
        return time_valid_;
    }

    // Монотонное время, миллисекунды.
    static uint64_t NowMs() noexcept
    {
        Lock lock;
        return now_ms_;
    }

    // Календарное время в секундах от 1970 года.
    // Если время не синхронизировано, возвращает 0.
    static uint64_t NowEpochSeconds() noexcept
    {
        Lock lock;
        if (!time_valid_) {
            return 0;
        }
        int64_t wall_ms = WallNowMsLocked(now_ms_);
        if (wall_ms < 0) {
            return 0;
        }
        return static_cast<uint64_t>(wall_ms) / 1000;
    }

    static Status NowEpochMs(uint64_t& epoch_sec, uint32_t& ms) noexcept
    {
        Lock lock;
        if (!time_valid_) {
            return Status::TimeNotSynced;
        }
        int64_t wall_ms = WallNowMsLocked(now_ms_);
        if (wall_ms < 0) {
            wall_ms = 0;
        }
        epoch_sec = static_cast<uint64_t>(wall_ms) / 1000;
        ms = static_cast<uint32_t>(static_cast<uint64_t>(wall_ms) % 1000);
        return Status::Ok;
    }

    static DateTime NowDateTime() noexcept
    {
        return FromEpochSeconds(NowEpochSeconds());
    }

    // Принудительно пересчитать и применить тик аппаратного таймера.
    // Полезно, если конфигурация менялась вне модуля или нужно обновить
    // энергопотребление после остановки одного таймера.
    static Status RefreshTick() noexcept
    {
        Lock lock;
        return UpdateTickLocked();
    }

private:
    static constexpr Size kTimerStorage =
        (MaxTimers > 0) ? static_cast<Size>(MaxTimers) : 1u;
    static constexpr Size kAlarmStorage =
        (MaxAlarms > 0) ? static_cast<Size>(MaxAlarms) : 1u;
    static constexpr Size kHookStorage =
        (MaxSystemHooks > 0) ? static_cast<Size>(MaxSystemHooks) : 1u;
    static constexpr uint64_t kMaxEpochSec =
        static_cast<uint64_t>(0x7FFFFFFFFFFFFFFFULL) / 1000ULL;

    struct TimerSlot
    {
        bool used = false;
        bool active = false;
        bool periodic = false;

        uint16_t gen = 0;
        uint16_t seq = 0;

        uint32_t period_ms = 0;
        uint64_t due_ms = 0;

        Callback_h cb = nullptr;
        void* arg = nullptr;
    };

    struct AlarmSlot
    {
        bool used = false;
        bool active = false;

        uint16_t gen = 0;
        uint16_t seq = 0;

        Repeat repeat = Repeat::Once;
        uint64_t due_sec = 0;

        Callback_h cb = nullptr;
        void* arg = nullptr;
    };

    struct HookSlot
    {
        bool used = false;
        Callback_h cb = nullptr;
        void* arg = nullptr;
    };

    static inline Config cfg_{};
    static inline volatile uint64_t now_ms_ = 0;
    static inline int64_t wall_offset_ms_ = 0;
    static inline uint32_t tick_ms_ = 1;
    static inline bool running_ = false;
    static inline bool time_valid_ = false;

    static inline TimerSlot timers_[kTimerStorage]{};
    static inline AlarmSlot alarms_[kAlarmStorage]{};
    static inline HookSlot hooks_[kHookStorage]{};

    // Small helpers
    static bool TimerValidLocked(const TimerHandle& h) noexcept
    {
        if (h.index >= MaxTimers) {
            return false;
        }
        const TimerSlot& s = timers_[h.index];
        return s.used && s.gen == h.gen;
    }

    static bool AlarmValidLocked(const AlarmHandle& h) noexcept
    {
        if (h.index >= MaxAlarms) {
            return false;
        }
        const AlarmSlot& a = alarms_[h.index];
        return a.used && a.gen == h.gen;
    }

    static int64_t WallNowMsLocked(uint64_t mono_ms) noexcept
    {
        return static_cast<int64_t>(mono_ms) + wall_offset_ms_;
    }

    static void RunSystemHooks() noexcept
    {
        for (uint8_t i = 0; i < MaxSystemHooks; ++i) {
            if (hooks_[i].used && hooks_[i].cb != nullptr) {
                hooks_[i].cb(hooks_[i].arg);
            }
        }
    }

    // Tick selection / hardware timer configuration
    static uint32_t FitTickLocked(uint32_t g) noexcept
    {
        if (g == 0) {
            g = cfg_.default_tick_ms;
        }
        if (g < cfg_.min_tick_ms) {
            return cfg_.min_tick_ms;
        }
        if (g <= cfg_.max_tick_ms) {
            return g;
        }
        // Если GCD больше максимального допустимого тика, ищем наибольший
        // делитель, который помещается в ограничения платформы.
        // Это редкая операция, она выполняется только при изменении набора
        // таймеров, не в каждом тике.
        for (uint32_t t = cfg_.max_tick_ms; t >= cfg_.min_tick_ms; --t) {
            if (g % t == 0) {
                return t;
            }
            if (t == cfg_.min_tick_ms) {
                break;
            }
        }
        return cfg_.min_tick_ms;
    }

    static uint32_t ComputeDesiredTickLocked() noexcept
    {
        uint32_t g = 0;
        for (uint8_t i = 0; i < MaxTimers; ++i) {
            const TimerSlot& s = timers_[i];
            if (s.used && s.active && s.period_ms > 0) {
                g = (g == 0) ? s.period_ms : Gcd(g, s.period_ms);
            }
        }
        bool has_alarm = false;
        for (uint8_t i = 0; i < MaxAlarms; ++i) {
            if (alarms_[i].used && alarms_[i].active) {
                has_alarm = true;
                break;
            }
        }
        if (has_alarm) {
            uint32_t alarm_tick = cfg_.alarm_tick_ms;
            if (alarm_tick == 0) {
                alarm_tick = cfg_.max_tick_ms;
            }
            g = (g == 0) ? alarm_tick : Gcd(g, alarm_tick);
        }
        return FitTickLocked(g);
    }

    static Status ApplyTickLocked(uint32_t new_tick_ms) noexcept
    {
        if (new_tick_ms < cfg_.min_tick_ms || new_tick_ms > cfg_.max_tick_ms) {
            return Status::Unavailable;
        }
        // Проверка, что выбранный тик делит все активные периоды.
        for (uint8_t i = 0; i < MaxTimers; ++i) {
            const TimerSlot& s = timers_[i];
            if (s.used && s.active && s.period_ms > 0) {
                if ((s.period_ms % new_tick_ms) != 0) {
                    return Status::Unavailable;
                }
            }
        }
        if (!platform_timer_can_period_ms(new_tick_ms)) {
            return Status::Unavailable;
        }
        if (running_) {
            platform_timer_stop();
        }
        tick_ms_ = new_tick_ms;
        platform_timer_set_period_ms(new_tick_ms);
        if (running_) {
            platform_timer_start();
        }
        return Status::Ok;
    }

    static Status UpdateTickLocked() noexcept
    {
        uint32_t desired = ComputeDesiredTickLocked();
        if (desired == tick_ms_) {
            return Status::Ok;
        }
        return ApplyTickLocked(desired);
    }

    // Processing
    static void ProcessTimers(uint64_t now) noexcept
    {
        for (uint8_t i = 0; i < MaxTimers; ++i) {
            if (!timers_[i].used || !timers_[i].active) {
                continue;
            }
            if (timers_[i].due_ms > now) {
                continue;
            }
            uint8_t fired = 0;
            while (timers_[i].used &&
                   timers_[i].active &&
                   timers_[i].due_ms <= now &&
                   fired < cfg_.max_catchup) {
                
                    Callback_h cb = timers_[i].cb;
                void* arg = timers_[i].arg;

                bool periodic = timers_[i].periodic;
                uint32_t period = timers_[i].period_ms;
                uint16_t seq = timers_[i].seq;
                if (cb != nullptr) {
                    cb(arg);
                }
                fired++;
                // Если callback изменил таймер (стоп, рестарт, удаление),
                // не трогаем его состояние.
                if (!(timers_[i].used &&
                      timers_[i].active &&
                      timers_[i].seq == seq)) {
                    break;
                }
                if (!periodic) {
                    timers_[i].active = false;
                    timers_[i].seq++;
                    break;
                }
                timers_[i].due_ms += period;
            }
            // Если таймер всё ещё сильно позади, не устраиваем burst,
            // а синхронизируем его относительно текущего момента.
            if (timers_[i].used &&
                timers_[i].active &&
                timers_[i].due_ms <= now) {
                timers_[i].due_ms = now + timers_[i].period_ms;
                timers_[i].seq++;
            }
        }
    }

    static void ProcessAlarms(uint64_t now) noexcept
    {
        if (!time_valid_) {
            return;
        }
        int64_t wall_ms = WallNowMsLocked(now);
        if (wall_ms < 0) {
            return;
        }
        uint64_t wall_sec = static_cast<uint64_t>(wall_ms) / 1000;
        for (uint8_t i = 0; i < MaxAlarms; ++i) {
            if (!alarms_[i].used || !alarms_[i].active) {
                continue;
            }
            if (alarms_[i].due_sec > wall_sec) {
                continue;
            }
            Callback_h cb = alarms_[i].cb;
            void* arg = alarms_[i].arg;

            Repeat repeat = alarms_[i].repeat;
            uint16_t seq = alarms_[i].seq;
            if (cb != nullptr) {
                cb(arg);
            }
            // Если callback отменил или изменил будильник, не трогаем его.
            if (!(alarms_[i].used &&
                  alarms_[i].active &&
                  alarms_[i].seq == seq)) {
                continue;
            }
            if (repeat == Repeat::Once) {
                alarms_[i].active = false;
                alarms_[i].seq++;
                continue;
            }
            uint64_t interval_sec = 0;
            if (repeat == Repeat::Daily) {
                interval_sec = 24 * 3600;
            } else if (repeat == Repeat::Weekly) {
                interval_sec = 7 * 24 * 3600;
            }
            if (interval_sec == 0) {
                alarms_[i].active = false;
                alarms_[i].seq++;
                continue;
            }
            // Если время резко уехало вперёд, не вызываем тысячи срабатываний.
            // Будильник сработает один раз и будет перенесён на следующий
            // корректный момент.
            uint64_t due = alarms_[i].due_sec;
            if (wall_sec >= due) {
                uint64_t delta = wall_sec - due + 1;
                uint64_t n = delta / interval_sec;
                due += n * interval_sec;
                if (due <= wall_sec) {
                    due += interval_sec;
                }
            }
            alarms_[i].due_sec = due;
            alarms_[i].seq++;
        }
    }
};

}