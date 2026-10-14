// power.cpp
// ---------------------------------------------------------------------------
// Реализация супервизора энергосбережения. Политика целиком здесь; всё, что
// зависит от железа (остановка ядра, STOP, источник пробуждения, частота),
// приходит через platform::Ops::power — поэтому политика проверяется на ПК
// на модельной платформе (tests/test_power.cpp).
// ---------------------------------------------------------------------------
#include "power.hpp"

#include "../core/task/sheduler.hpp"

namespace embed::power {

namespace {

inline const platform::Ops& P() noexcept { return platform::ops(); }

inline constexpr uint32_t cap(platform::PowerCaps c) noexcept {
    return static_cast<uint32_t>(c);
}

} // namespace

uint32_t project_load_percent(uint32_t load_percent,
                              uint32_t from_hz,
                              uint32_t to_hz) noexcept {
    // Нулевая частота — «неизвестно»: честнее вернуть вход, чем выдумать число.
    if (from_hz == 0u || to_hz == 0u || load_percent == 0u) return load_percent;

    // 64-битная арифметика: load_percent * from_hz на 32 битах переполняется
    // (100 * 72 000 000 > 2^32) и дал бы «легко уложимся» вместо «не влезаем».
    const uint64_t projected = (static_cast<uint64_t>(load_percent) * from_hz) / to_hz;
    return (projected > 1000u) ? 1000u : static_cast<uint32_t>(projected);
}

// ─────────────────────────────── Инициализация ──────────────────────────────

Status Supervisor::apply_config(const Config& cfg) noexcept {
    const platform::PowerOps* pw = P().power;
    // Контракт требует непустую таблицу, но проверка остаётся: цена ошибки —
    // разыменование nullptr в idle-задаче, то есть падение в простое.
    if (pw == nullptr || pw->caps == nullptr) return Status::NotSupported;

    Config c = cfg;
    if (c.window_ms == 0u) c.window_ms = 1000u;
    if (c.deep_sleep_min_ms == 0u) c.deep_sleep_min_ms = 1u;

    const uint32_t caps = pw->caps();

    if (c.allow_deep_sleep && (caps & cap(platform::PowerCaps::DeepSleep)) == 0u) {
        // Платформа не умеет глубокий сон: молча выключить его значило бы
        // соврать приложению в отчёте «почему не спим».
        return Status::NotSupported;
    }

    if (c.profile != profile_) {
        // Смена частоты допустима ТОЛЬКО до старта системного тика: SysTick,
        // делители USART/I2C и прескалер АЦП настроены под текущую частоту.
        if (config::OsTimer::TimeAvailable()) return Status::Busy;

        if ((caps & cap(platform::PowerCaps::StaticProfile)) == 0u) {
            return Status::NotSupported;
        }
        // set_profile обязан быть идемпотентным: «привести к профилю» при
        // уже установленном профиле — это no-op, а не перезапуск PLL.
        const uint32_t hz = pw->set_profile(static_cast<uint32_t>(c.profile));
        if (hz == 0u) return Status::NotSupported;
        sysclk_hz_ = hz;
        profile_   = c.profile;
    } else if (sysclk_hz_ == 0u) {
        // Профиль уже тот, что нужен: частоту достаточно узнать, а не менять.
        sysclk_hz_ = (pw->sysclk_hz != nullptr) ? pw->sysclk_hz() : 0u;
    }

    Lock lock;
    cfg_ = c;
    return Status::Ok;
}

Status Supervisor::init(const Config& cfg) noexcept {
    if (inited_) return Status::AlreadyExists;

    const Status st = apply_config(cfg);
    if (st != Status::Ok) return st;

    Lock lock;

    window_ms_      = 0u;
    window_busy_ms_ = 0u;
    window_idle_ms_ = 0u;
    windows_        = 0u;
    load_sum_       = 0u;
    last_load_      = 0u;
    peak_load_      = 0u;

    sleep_entries_      = 0u;
    deep_entries_       = 0u;
    deep_ms_            = 0u;
    refused_short_      = 0u;
    refused_keep_awake_ = 0u;
    refused_no_source_  = 0u;
    deep_failed_        = 0u;
    last_mode_          = Mode::Run;

    keep_awake_   = 0u;
    wake_hint_ms_ = 0u;

    inited_ = true;
    return Status::Ok;
}

Status Supervisor::configure(const Config& cfg) noexcept {
    if (!inited_) return Status::Unavailable;
    // Статистику не сбрасываем: приложение меняет ПОЛИТИКУ, а не начинает
    // измерение заново — иначе «почему не спали» потеряло бы историю.
    return apply_config(cfg);
}

// ──────────────────────────────── Удержание ─────────────────────────────────

Supervisor::Lease Supervisor::keep_awake() noexcept {
    if (!inited_) return Lease(false);

    Lock lock;
    ++keep_awake_;
    return Lease(true);
}

void Supervisor::Lease::release() noexcept {
    if (!held_) return;
    held_ = false;

    Supervisor& s = Supervisor::instance();
    Lock lock;
    if (s.keep_awake_ > 0u) --s.keep_awake_;
}

void Supervisor::set_wake_hint_ms(uint32_t ms) noexcept {
    Lock lock;
    wake_hint_ms_ = ms;
}

Status Supervisor::add_sleep_limit(const SleepLimit& limit, void* ctx) noexcept {
    if (limit.limit_ms == nullptr) return Status::InvalidArg;

    Lock lock;
    for (uint32_t i = 0u; i < limit_count_; ++i) {
        if (limit_[i] == &limit && limit_ctx_[i] == ctx) return Status::AlreadyExists;
    }
    if (limit_count_ >= kMaxSleepLimits) return Status::NoMemory;

    limit_[limit_count_]     = &limit;
    limit_ctx_[limit_count_] = ctx;
    ++limit_count_;
    return Status::Ok;
}

void Supervisor::notify_slept(uint32_t slept_ms) noexcept {
    // Снимок таблицы под критической секцией, вызовы — уже без неё: обработчик
    // источника может быть сколь угодно «тяжёлым» (пересчёт сроков, кормление
    // сторожевого таймера) и не должен выполняться с выключенными прерываниями.
    const SleepLimit* fn[kMaxSleepLimits]{};
    void*             ctx[kMaxSleepLimits]{};
    uint32_t          n = 0u;
    {
        Lock lock;
        n = limit_count_;
        for (uint32_t i = 0u; i < n; ++i) {
            fn[i]  = limit_[i];
            ctx[i] = limit_ctx_[i];
        }
    }
    for (uint32_t i = 0u; i < n; ++i) {
        if (fn[i] != nullptr && fn[i]->on_slept != nullptr) {
            fn[i]->on_slept(ctx[i], slept_ms);
        }
    }
}

// ───────────────────────────────── Горизонт ─────────────────────────────────

uint32_t Supervisor::horizon_ms() noexcept {
    Lock lock;
    return horizon_locked();
}

uint32_t Supervisor::horizon_locked() const noexcept {
    // Известных сроков нет — «можно спать сколько угодно»; ограничение
    // наложит платформа (гарантия источника пробуждения).
    uint32_t h = kNoDeadline;

    const uint32_t timer_ms = config::OsTimer::NextDeadlineMs();
    if (timer_ms < h) h = timer_ms;

    // Спящие задачи: их срок измеряется в тиках ОС.
    const uint32_t ticks = scheduler::instance().next_sleep_ticks();
    if (ticks != kNoDeadline) {
        uint32_t tick_ms = config::OsTimer::TickMs();
        if (tick_ms == 0u) tick_ms = 1u;
        const uint64_t ms = static_cast<uint64_t>(ticks) * tick_ms;
        const uint32_t capped = (ms >= kNoDeadline) ? kNoDeadline
                                                   : static_cast<uint32_t>(ms);
        if (capped < h) h = capped;
    }

    if (wake_hint_ms_ != 0u && wake_hint_ms_ < h) h = wake_hint_ms_;

    // Источники, у которых свой срок, невидимый ядру (сторожевой таймер:
    // он считает и в STOP, а кормить его там некому).
    for (uint32_t i = 0u; i < limit_count_; ++i) {
        const SleepLimit* l = limit_[i];
        if (l == nullptr || l->limit_ms == nullptr) continue;
        const uint32_t limit = l->limit_ms(limit_ctx_[i]);
        if (limit < h) h = limit;
    }
    return h;
}

// ────────────────────────────────── Учёт ────────────────────────────────────

void Supervisor::close_window_locked() noexcept {
    const uint32_t total = window_ms_;
    if (total == 0u) return;

    const uint32_t load = (window_busy_ms_ * 100u) / total;

    last_load_ = static_cast<uint8_t>(load > 100u ? 100u : load);
    if (last_load_ > peak_load_) peak_load_ = last_load_;
    load_sum_ += last_load_;
    ++windows_;

    window_ms_      = 0u;
    window_busy_ms_ = 0u;
    window_idle_ms_ = 0u;
}

void Supervisor::on_tick() noexcept {
    if (!inited_) return;

    const uint32_t tick_ms = config::OsTimer::TickMs();
    if (tick_ms == 0u) return;

    // Единственное измерение загрузки: исполнялась ли в момент прерывания
    // idle-задача. Дёшево (сравнение указателя) и не зависит от того, сколько
    // тактов занял сам обработчик.
    const bool idle = scheduler::instance().idle_running();

    Lock lock;
    if (window_ms_ > kNoDeadline - tick_ms) {
        // Счётчик окон не должен переполняться, если окно очень длинное.
        close_window_locked();
    }
    window_ms_ += tick_ms;
    if (idle) window_idle_ms_ += tick_ms;
    else      window_busy_ms_ += tick_ms;

    if (window_ms_ >= cfg_.window_ms) close_window_locked();
}

void Supervisor::account_deep_sleep(uint32_t slept_ms) noexcept {
    {
        Lock lock;
        ++deep_entries_;
        deep_ms_   += slept_ms;
        last_mode_  = Mode::DeepSleep;
    }

    // SysTick в глубоком сне не считал: монотонное время ОС продвигаем на
    // ИЗМЕРЕННОЕ платформой время, а не на запрошенное (будильник может
    // сработать позже из-за гранулярности).
    const uint32_t tick_ms = config::OsTimer::TickMs();
    (void)config::OsTimer::AdvanceMs(slept_ms);

    if (tick_ms != 0u) {
        // Спящие задачи измеряют срок в тиках: без этого «догона» задача,
        // чей срок истёк внутри сна, ждала бы ещё столько же тиков.
        scheduler::instance().advance_ticks(slept_ms / tick_ms);
    }

    // Источники со своим сроком (сторожевой таймер) узнают о сне ПОСЛЕ
    // продвижения времени: им нужно пересчитать сроки от текущего момента.
    notify_slept(slept_ms);
}

// ────────────────────────────────── Простой ─────────────────────────────────

Supervisor::Plan Supervisor::plan_idle() noexcept {
    const platform::PowerOps* pw = P().power;
    Plan plan{};

    // Sleep доступен всегда, когда платформа его заявила: будит любое
    // прерывание (в том числе системный тик).
    plan.mode = ((pw->caps() & cap(platform::PowerCaps::Sleep)) != 0u) ? Mode::Sleep
                                                                      : Mode::Run;

    if (!cfg_.allow_deep_sleep) return plan;

    // Гарантия пробуждения: сколько платформа обещает проспать при текущем
    // вооружённом источнике. 0 — источник не готов, спать нельзя.
    const uint32_t plat_horizon = (pw->deep_sleep_horizon_ms != nullptr)
                                ? pw->deep_sleep_horizon_ms() : 0u;
    if (plat_horizon == 0u) {
        Lock lock;
        ++refused_no_source_;
        return plan;
    }

    Lock lock;
    if (keep_awake_ != 0u) {
        ++refused_keep_awake_;
        return plan;
    }

    uint32_t h = horizon_locked();
    if (h > plat_horizon) h = plat_horizon;
    if (cfg_.deep_sleep_max_ms != 0u && h > cfg_.deep_sleep_max_ms) {
        h = cfg_.deep_sleep_max_ms;
    }

    // Окупаемость: вход/выход из STOP стоит так же, как несколько
    // миллисекунд работы, поэтому короткий простой в STOP не экономнее WFI.
    // Гранулярность добавляется к порогу: при секундном будильнике спать
    // «на 10 мс» физически невозможно.
    const uint32_t granularity = (pw->deep_sleep_granularity_ms != nullptr)
                               ? pw->deep_sleep_granularity_ms() : 0u;
    if (h < cfg_.deep_sleep_min_ms + granularity) {
        ++refused_short_;
        return plan;
    }

    plan.mode     = Mode::DeepSleep;
    plan.sleep_ms = h;
    return plan;
}

void Supervisor::on_idle() noexcept {
    const platform::Ops& p = P();

    // До init() супервизор не управляет ничем: прежнее поведение платформы.
    if (!inited_ || p.power == nullptr) {
        if (p.idle) p.idle();
        return;
    }

    const Plan plan = plan_idle();

    if (plan.mode == Mode::DeepSleep) {
        // ВАЖНО: никаких блокировок вокруг вызова платформы. Lock маскирует
        // прерывания, а из STOP ядро выводит именно прерывание.
        const uint32_t slept = (p.power->deep_sleep != nullptr)
                             ? p.power->deep_sleep(plan.sleep_ms) : 0u;
        if (slept > 0u) {
            account_deep_sleep(slept);
            return;
        }
        Lock lock;
        ++deep_failed_;
        last_mode_ = Mode::Run;
    }

    if (plan.mode == Mode::Run) return;

    {
        Lock lock;
        ++sleep_entries_;
        last_mode_ = Mode::Sleep;
    }
    if (p.idle) p.idle();
}

// ──────────────────────────────── Наблюдение ────────────────────────────────

Stats Supervisor::stats() noexcept {
    Lock lock;

    Stats s{};
    s.windows           = windows_;
    s.window_ms         = window_ms_;
    s.busy_ms           = window_busy_ms_;
    s.idle_ms           = window_idle_ms_;
    s.load_percent      = last_load_;
    s.peak_load_percent = peak_load_;
    // 64-битное деление — сотни тактов на Cortex-M3, но stats() вызывается
    // из отчёта/диагностики, а не в горячем пути.
    s.avg_load_percent  = (windows_ == 0u)
                        ? 0u
                        : static_cast<uint8_t>(load_sum_ / windows_);

    s.sleep_entries      = sleep_entries_;
    s.deep_sleep_entries = deep_entries_;
    s.deep_sleep_ms      = deep_ms_;

    s.refused_short      = refused_short_;
    s.refused_keep_awake = refused_keep_awake_;
    s.refused_no_source  = refused_no_source_;
    s.deep_sleep_failed  = deep_failed_;
    return s;
}

} // namespace embed::power
