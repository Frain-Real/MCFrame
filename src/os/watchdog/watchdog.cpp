// watchdog.cpp
// ---------------------------------------------------------------------------
// Реализация сервиса сторожевого таймера. Политика целиком здесь; всё, что
// зависит от железа (запуск таймера, кормление, резервные регистры, причина
// сброса), приходит через platform::Ops::watchdog — поэтому политика
// проверяется на ПК на модельной платформе (tests/test_watchdog.cpp).
// ---------------------------------------------------------------------------
#include "watchdog.hpp"

#include "../power/power.hpp"   // ограничение длительности глубокого сна

namespace embed::watchdog {

namespace {

inline const platform::Ops& P() noexcept { return platform::ops(); }

/// Источник ограничения сна для супервизора энергосбережения.
///
/// Логика пары функций отражает физику: пока система спит, НИКТО не может
/// отметиться, поэтому во сне программное слежение «на паузе», а за то, что
/// устройство вообще проснётся, отвечает аппаратный таймер. Отсюда:
///   * limit_ms — сколько можно не просыпаться, чтобы аппаратный таймер не
///     сработал (плюс запас на пробуждение);
///   * on_slept — после пробуждения: немедленно покормить таймер и начать
///     отсчёт сроков участников заново.
const power::Supervisor::SleepLimit kSleepLimit{
    /* limit_ms */ &embed::watchdog::Service::sleep_limit_hook,
    /* on_slept */ &embed::watchdog::Service::slept_hook,
};

inline constexpr uint32_t wd_cap(platform::WatchdogCaps c) noexcept {
    return static_cast<uint32_t>(c);
}

inline constexpr uint32_t cause_bit(platform::ResetCause c) noexcept {
    return static_cast<uint32_t>(c);
}

/// Максимум двух значений без ветвлений «кто больше» в вызывающем коде.
inline constexpr uint32_t umax(uint32_t a, uint32_t b) noexcept {
    return (a > b) ? a : b;
}

/// Монотонное время ОС в 32 битах.
///
/// Обёртка через 49.7 суток безопасна: все сравнения сроков в модуле —
/// РАЗНОСТНЫЕ (см. Service::time_reached), а не «больше/меньше абсолютного
/// значения», поэтому переход через ноль ничего не ломает. 64-битные
/// сравнения стоили бы на Cortex-M3 больше тактов, чем весь остальной тик.
inline uint32_t now_ms() noexcept {
    return static_cast<uint32_t>(config::OsTimer::NowMs());
}

inline uint32_t granularity_ms() noexcept {
    const platform::Ops& p = P();
    if (p.power == nullptr || p.power->deep_sleep_granularity_ms == nullptr) return 0u;
    return p.power->deep_sleep_granularity_ms();
}

} // namespace

// ─────────────────────────────── Участник ───────────────────────────────────

Status Participant::init(const char* name, uint32_t timeout_ms,
                         uint32_t min_interval_ms) noexcept {
    if (name == nullptr || timeout_ms == 0u) return Status::InvalidArg;
    if (min_interval_ms >= timeout_ms) return Status::InvalidArg;
    if (used_) return Status::AlreadyExists;

    name_            = name;
    timeout_ms_      = timeout_ms;
    min_interval_ms_ = min_interval_ms;
    misses_.store(0u, MemoryOrderRelaxed);
    checks_.store(0u, MemoryOrderRelaxed);
    failed_.store(false, MemoryOrderSeqCst);
    last_ms_.store(now_ms(), MemoryOrderRelaxed);

    // Флаг «зарегистрирован» выставляем ДО попадания в таблицу сервиса: иначе
    // прерывание тика могло бы увидеть слот с ещё не заполненными полями.
    used_ = true;
    const Status st = Service::instance().register_participant(*this);
    if (st != Status::Ok) {
        used_ = false;
        return st;
    }
    return Status::Ok;
}

Status Participant::check_in() noexcept {
    if (!used_) return Status::NotFound;

    const uint32_t now  = now_ms();
    const uint32_t prev = last_ms_.load(MemoryOrderRelaxed);
    (void)checks_.fetch_add(1u, MemoryOrderRelaxed);

    // Отметка раньше min_interval_ms — такой же признак беды, как опоздание:
    // задача крутится в цикле, но работу не делает. Считаем нарушением, но
    // отметку ПРИНИМАЕМ: иначе одна ранняя отметка превратилась бы в сброс по
    // просрочке, то есть в ложное срабатывание.
    bool early = false;
    if (min_interval_ms_ != 0u && !Service::time_reached(now, prev + min_interval_ms_)) {
        early = true;
        Service::instance().count_early();
    }

    last_ms_.store(now, MemoryOrderRelaxed);
    return early ? Status::AlreadyExists : Status::Ok;
}

void Participant::fail() noexcept {
    if (!used_) return;
    failed_.store(true, MemoryOrderSeqCst);
}

uint32_t Participant::age_ms() const noexcept {
    const uint32_t now = now_ms();
    return now - last_ms_.load(MemoryOrderRelaxed);
}

// ─────────────────────────────── Сервис ─────────────────────────────────────

Status Service::init(const Config& cfg) noexcept {
    if (inited_) return Status::AlreadyExists;

    const platform::WatchdogOps* wd = P().watchdog;
    // Контракт требует непустую таблицу, но цена ошибки — разыменование
    // nullptr в прерывании тика, то есть падение в самом «защитном» коде.
    if (wd == nullptr || wd->caps == nullptr) return Status::NotSupported;
    if (cfg.check_period_ms == 0u) return Status::InvalidArg;

    const uint32_t caps = wd->caps();
    if (cfg.mode == Mode::Reset && (caps & wd_cap(platform::WatchdogCaps::Hardware)) == 0u) {
        // Сбрасывать нечем: молча работать «вхолостую» — обман.
        return Status::NotSupported;
    }

    cfg_ = cfg;

    // Сколько времени сервису нужно, чтобы проснуться и заметить просрочку.
    // В это входит гранулярность источника пробуждения: если платформа будит
    // из глубокого сна с точностью до секунды, то «проснуться на 100 мс
    // раньше срока» невозможно физически.
    sleep_safety_ms_ = (cfg.sleep_safety_ms != 0u)
                     ? cfg.sleep_safety_ms
                     : (cfg.check_period_ms + granularity_ms());

    // Причина последнего сброса читается ОДИН РАЗ: чтение RCC_CSR сбрасывает
    // флаги (так устроено железо), поэтому второй вызов вернул бы пустоту.
    if ((caps & wd_cap(platform::WatchdogCaps::ResetCause)) != 0u
        && wd->read_reset_cause != nullptr) {
        reset_cause_ = wd->read_reset_cause();
    }

    // Аппаратный таймер считает и в глубоком сне, а кормить его там некому:
    // сообщаем супервизору энергосбережения, сколько можно не просыпаться.
    // Регистрируем ДО start(): пока железо не запущено, ограничение равно
    // «без ограничения», поэтому порядок безопасен в любую сторону.
    // Делаем это ДО записи номера загрузки, чтобы отказ не оставлял в
    // резервных регистрах следов «состоявшейся» инициализации.
    const Status lst = power::Supervisor::instance().add_sleep_limit(kSleepLimit, nullptr);
    if (lst != Status::Ok) return lst;

    // ── Номер загрузки ──
    // Счётчик живёт в резервных регистрах и переживает сброс: именно он
    // позволяет отличить «просрочка была в прошлой загрузке» от «отметка
    // осталась с позапрошлой и к текущей загрузке отношения не имеет».
    const uint16_t magic = note_read(kNoteIdxMagic);
    boot_count_ = ((magic == kNoteMagic) ? note_read(kNoteIdxBoot) : 0u) + 1u;
    (void)note_write(kNoteIdxMagic, kNoteMagic);
    (void)note_write(kNoteIdxBoot, static_cast<uint16_t>(boot_count_));

    inited_ = true;
    return Status::Ok;
}

uint32_t Service::sleep_limit_hook(void* ctx) noexcept {
    (void)ctx;
    return Service::instance().sleep_limit_ms();
}

void Service::slept_hook(void* ctx, uint32_t slept_ms) noexcept {
    (void)ctx;
    Service::instance().on_slept(slept_ms);
}

void Service::on_slept(uint32_t slept_ms) noexcept {
    Lock lock;
    if (!inited_ || !running_) return;

    slept_ms_ += slept_ms;

    // Уже поймали просрочку (решение латчировано) — кормить НЕЛЬЗЯ, иначе
    // глубокий сон стал бы способом «переждать» нарушение и отменить сброс.
    if (warned_) return;

    const uint32_t now = now_ms();

    // Во сне отметиться было некому: сроки участников начинаются заново.
    for (uint32_t i = 0; i < participants_; ++i) {
        slots_[i]->last_ms_.store(now, MemoryOrderRelaxed);
    }

    // И сразу кормим: аппаратный таймер всё это время считал.
    kick(now);
    ++deep_sleeps_;
}

Status Service::register_participant(Participant& p) noexcept {
    if (!inited_) return Status::Unavailable;

    Lock lock;
    // После start() аппаратный таймаут уже рассчитан по зарегистрированным
    // срокам — новый участник с большим сроком сделал бы его недостаточным.
    if (running_) return Status::Busy;
    if (participants_ >= MCFRAME_WATCHDOG_SLOTS) return Status::NoMemory;

    for (uint32_t i = 0; i < participants_; ++i) {
        if (slots_[i] == &p) return Status::AlreadyExists;
    }
    slots_[participants_++] = &p;
    return Status::Ok;
}

Status Service::start() noexcept {
    if (!inited_) return Status::Unavailable;
    if (running_) return Status::AlreadyExists;

    const platform::WatchdogOps* wd = P().watchdog;
    const uint32_t now = now_ms();

    // Сроки начинают течь с этого момента: время инициализации приложения
    // (до start()) не должно засчитываться как просрочка.
    for (uint32_t i = 0; i < participants_; ++i) {
        slots_[i]->last_ms_.store(now, MemoryOrderRelaxed);
    }

    last_kick_ms_   = now;
    stall_until_ms_ = 0u;
    fault_noted_    = false;
    warned_         = false;
    running_        = true;

    const uint32_t caps = wd->caps();
    if (cfg_.mode == Mode::Off || (caps & wd_cap(platform::WatchdogCaps::Hardware)) == 0u) {
        // Программное слежение: предупреждения и статистика есть, сбрасывать
        // нечем. Это осознанный режим стенда/отладки.
        hardware_      = false;
        hw_timeout_ms_ = 0u;
        kick_period_ms_ = cfg_.check_period_ms;
        return Status::Ok;
    }

    // ── Аппаратный таймаут ──
    // Берём МАКСИМАЛЬНЫЙ срок участников (аппаратный таймер один на всех) и
    // добавляем запас. Запас обязан вмещать: задержку обнаружения (период
    // проверки), время до записи диагностики и возврат из глубокого сна.
    uint32_t max_timeout = 0u;
    for (uint32_t i = 0; i < participants_; ++i) {
        max_timeout = umax(max_timeout, slots_[i]->timeout_ms_);
    }
    if (max_timeout == 0u) {
        // Участников нет: сторож следит за самим сервисом («ядро живо»).
        max_timeout = cfg_.check_period_ms;
    }

    const uint32_t min_margin = 2u * cfg_.check_period_ms + sleep_safety_ms_;
    uint32_t margin = (cfg_.hw_margin_ms != 0u) ? cfg_.hw_margin_ms : min_margin;
    if (margin < min_margin) {
        // Меньший запас означал бы сброс раньше, чем сервис успеет записать
        // причину: сторож работал бы, а диагностики не было бы.
        margin = min_margin;
    }

    const uint32_t requested = max_timeout + margin;
    const uint32_t hw = (wd->closest_timeout_ms != nullptr)
                      ? wd->closest_timeout_ms(requested) : 0u;
    if (hw == 0u || hw < max_timeout + cfg_.check_period_ms) {
        running_ = false;
        return Status::NotSupported;
    }

    if (wd->start == nullptr || !wd->start(hw)) {
        running_ = false;
        return Status::Unavailable;
    }

    hw_timeout_ms_  = hw;
    kick_period_ms_ = cfg_.check_period_ms;
    hardware_       = true;

    // Первое кормление сразу: между start() и первым тиком пройдёт до одного
    // периода, а таймер уже считает.
    wd->kick();
    ++kicks_;
    return Status::Ok;
}

Status Service::stop() noexcept {
    if (!inited_ || !running_) return Status::Unavailable;

    const platform::WatchdogOps* wd = P().watchdog;
    const uint32_t caps = wd->caps();
    if ((caps & wd_cap(platform::WatchdogCaps::Stoppable)) == 0u
        || wd->stop == nullptr) {
        // Независимый таймер на STM32F1 остановить нельзя — и это правильно:
        // «выключить сторожа на время» почти всегда означает «выключить
        // защиту там, где она нужнее всего».
        return Status::NotSupported;
    }
    if (!wd->stop()) return Status::Unavailable;

    hardware_      = false;
    hw_timeout_ms_ = 0u;
    running_       = false;
    return Status::Ok;
}

// ────────────────────────────── Тик и политика ──────────────────────────────

void Service::kick(uint32_t now) noexcept {
    if (hardware_ && P().watchdog->kick != nullptr) {
        P().watchdog->kick();
        ++kicks_;
    }
    // last_kick_ms_ обновляем и в программном режиме: по нему считается
    // «сколько ещё можно спать», а он не должен зависеть от наличия железа.
    last_kick_ms_ = now;
}

void Service::on_tick() noexcept {
    if (!inited_ || !running_) return;

    const uint32_t now = now_ms();
    Lock lock;

    ++checks_;

    // Открытое окно длительной операции: кормим железо, но не судим сроки.
    const bool stalled = (stall_until_ms_ != 0u) && !time_reached(now, stall_until_ms_);

    bool     miss       = false;
    uint32_t worst      = kNoParticipant;
    uint32_t worst_late = 0u;

    for (uint32_t i = 0; i < participants_; ++i) {
        Participant& p = *slots_[i];

        if (p.failed_.load(MemoryOrderSeqCst)) {
            miss = true;
            if (worst == kNoParticipant) worst = i;
            continue;
        }

        const uint32_t age = now - p.last_ms_.load(MemoryOrderRelaxed);
        if (age <= p.timeout_ms_) continue;         // жив

        const uint32_t late = age - p.timeout_ms_;
        miss = true;
        (void)p.misses_.fetch_add(1u, MemoryOrderRelaxed);
        if (late > worst_late) {
            worst_late = late;
            worst      = i;
        }
    }

    // ── Защёлка ──
    // Решение о просрочке принимается ОДИН раз и не отменяется само: иначе
    // участник, «оживший» после просрочки, отменил бы сброс, и сторож
    // маскировал бы ровно то, ради чего он поставлен. Снять защёлку может
    // только приложение (rearm) или сброс устройства.
    if (warned_) return;

    if (!miss || stalled) {
        if (time_reached(now, last_kick_ms_ + kick_period_ms_)) kick(now);
        return;
    }

    // ── Просрочка ──
    // Кормление прекращается: устройство сбросит аппаратный таймер.
    ++misses_;
    if (worst_late > max_late_ms_) max_late_ms_ = worst_late;

    if (!warned_) {
        warned_ = true;
        ++warnings_;
        note_fault(worst, worst_late);

        if (cfg_.mode == Mode::Warning && cfg_.on_warning != nullptr) {
            // Колбэк вызывается из прерывания тика: он обязан быть коротким —
            // без блокировок, сна и обмена по шинам. Его задача — успеть
            // сообщить наружу (лог, аварийный флаг) до сброса.
            cfg_.on_warning(cfg_.warning_arg);
        }
    }
    // Железо не кормим: устройство сбросит аппаратный таймер через свой срок.
}

void Service::count_early() noexcept {
    Lock lock;
    ++early_checks_;
}

void Service::note_fault(uint32_t participant, uint32_t late_ms) noexcept {
    if (fault_noted_) return;
    fault_noted_ = true;

    const uint16_t code = (participant == kNoParticipant)
                        ? static_cast<uint16_t>(0xFFFFu)
                        : static_cast<uint16_t>(participant + 1u);
    (void)note_write(kNoteIdxFault, code);
    (void)note_write(kNoteIdxLate, static_cast<uint16_t>(late_ms > 0xFFFFu ? 0xFFFFu
                                                                       : late_ms));
    // В какой загрузке это случилось: по ней следующая загрузка отличит
    // «свою» отметку от устаревшей.
    (void)note_write(kNoteIdxFBoot, static_cast<uint16_t>(boot_count_));
}

// ────────────────────────── Резервные регистры ──────────────────────────────

bool Service::note_write(uint16_t index, uint16_t value) noexcept {
    const platform::WatchdogOps* wd = P().watchdog;
    if (wd->note_write == nullptr) return false;
    return wd->note_write(static_cast<uint32_t>(cfg_.note_base) + index, value);
}

uint16_t Service::note_read(uint16_t index) noexcept {
    const platform::WatchdogOps* wd = P().watchdog;
    if (wd->note_read == nullptr) return 0u;
    return wd->note_read(static_cast<uint32_t>(cfg_.note_base) + index);
}

// ─────────────────────── Длительные операции и сон ──────────────────────────

Status Service::allow_stall(uint32_t ms) noexcept {
    if (!inited_) return Status::Unavailable;

    if (ms == 0u || cfg_.max_stall_ms == 0u || ms > cfg_.max_stall_ms) {
        Lock lock;
        ++refused_stalls_;
        return Status::NotSupported;
    }

    const uint32_t now = now_ms();
    Lock lock;
    const uint32_t target = now + ms;
    // Продлеваем только вперёд: два вложенных разрешения не должны укорачивать
    // окно друг друга.
    if (stall_until_ms_ == 0u || time_reached(now, stall_until_ms_) ||
        static_cast<int32_t>(target - stall_until_ms_) > 0) {
        stall_until_ms_ = target;
    }
    ++stalls_;
    return Status::Ok;
}

uint32_t Service::stall_left_ms() noexcept {
    Lock lock;
    const uint32_t now = now_ms();
    if (stall_until_ms_ == 0u || time_reached(now, stall_until_ms_)) return 0u;
    return stall_until_ms_ - now;
}

uint32_t Service::sleep_limit_ms() noexcept {
    Lock lock;
    // Сбрасывать нечему — спать можно сколько угодно.
    if (!inited_ || !running_ || !hardware_) return kNoDeadline;

    // Во время длительной операции кормить таймер некому (сервис не тикает во
    // сне), поэтому спать нельзя вовсе.
    const uint32_t now = now_ms();
    if (stall_until_ms_ != 0u && !time_reached(now, stall_until_ms_)) return 0u;

    // Аппаратный таймер сбросит устройство в last_kick + hw_timeout. Проснуться
    // нужно РАНЬШЕ этого срока на запас: сам запас уже учитывает и период
    // проверки, и гранулярность источника пробуждения.
    const uint32_t deadline = last_kick_ms_ + hw_timeout_ms_;
    if (time_reached(now, deadline)) return 0u;

    const uint32_t left = deadline - now;
    return (left > sleep_safety_ms_) ? (left - sleep_safety_ms_) : 0u;
}

// ─────────────────────────────── Наблюдение ─────────────────────────────────

Stats Service::stats() noexcept {
    Lock lock;

    Stats s{};
    s.participants   = participants_;
    s.checks         = checks_;
    s.kicks          = kicks_;
    s.misses         = misses_;
    s.early_checks   = early_checks_;
    s.warnings       = warnings_;
    s.stalls         = stalls_;
    s.refused_stalls = refused_stalls_;
    s.deep_sleeps    = deep_sleeps_;
    s.slept_ms       = slept_ms_;
    s.rearms         = rearms_;
    s.max_late_ms    = max_late_ms_;
    s.hardware       = hardware_;
    return s;
}

Fault Service::last_fault() noexcept {
    Lock lock;

    Fault f{};
    f.cause      = reset_cause_;
    f.boot_count = boot_count_;

    // Отметку читаем ЛЕНИВО, а не кэшируем при init(): во-первых, она лежит в
    // регистрах, которые переживают сброс, и приложение вправе прочитать её в
    // любой момент; во-вторых, так видно, что между чтениями её никто не
    // «портит».
    const uint16_t code  = note_read(kNoteIdxFault);
    const uint16_t late  = note_read(kNoteIdxLate);
    const uint16_t stamp = note_read(kNoteIdxFBoot);

    const bool wd_reset =
        ((reset_cause_ & cause_bit(platform::ResetCause::IndependentWatchdog)) != 0u) ||
        ((reset_cause_ & cause_bit(platform::ResetCause::WindowWatchdog)) != 0u);

    // Отметка относится к ПРЕДЫДУЩЕЙ загрузке: тогда и только тогда она —
    // причина текущего запуска. Отметка «позапрошлой» загрузки игнорируется
    // (иначе одна старая просрочка «объясняла» бы все последующие запуски).
    const bool fresh = (code != 0u) && (stamp != 0u) &&
                       (static_cast<uint32_t>(stamp) + 1u == boot_count_);

    if (fresh) {
        f.valid       = true;
        f.participant = (code == 0xFFFFu) ? kNoParticipant
                                          : static_cast<uint32_t>(code - 1u);
        f.late_ms     = late;
    } else if (wd_reset) {
        // Сброс от сторожа, а отметки нет: значит, отметить не успели —
        // например, не работало само прерывание тика, и записать причину было
        // некому. Это тоже информация, и молчать о ней нельзя.
        f.valid = true;
    }
    return f;
}

Status Service::clear_fault() noexcept {
    if (!inited_) return Status::Unavailable;

    Lock lock;
    (void)note_write(kNoteIdxFault, 0u);
    (void)note_write(kNoteIdxLate, 0u);
    (void)note_write(kNoteIdxFBoot, 0u);
    return Status::Ok;
}

Status Service::rearm() noexcept {
    if (!inited_ || !running_) return Status::Unavailable;

    const uint32_t now = now_ms();
    Lock lock;
    if (!warned_ && !fault_noted_) return Status::AlreadyExists;

    for (uint32_t i = 0; i < participants_; ++i) {
        slots_[i]->last_ms_.store(now, MemoryOrderRelaxed);
        // Снимаем и признак отказа: приложение, которое вызвало rearm(),
        // утверждает, что подсистема перезапущена и снова работает.
        slots_[i]->failed_.store(false, MemoryOrderSeqCst);
    }
    warned_      = false;
    fault_noted_ = false;
    ++rearms_;

    // Сразу кормим: аппаратный таймер всё это время не кормили, и именно
    // поэтому rearm() имеет смысл только как осознанное действие приложения.
    kick(now);
    return Status::Ok;
}

} // namespace embed::watchdog
