// tests/test_watchdog.cpp
// ---------------------------------------------------------------------------
// Сервис сторожевого таймера: слежение за участниками, кормление железа,
// интеграция с глубоким сном и посмертная диагностика.
//
// ЧТО ЗДЕСЬ ГЛАВНОЕ
// Сторожевой таймер — единственная подсистема, которая существует ради того,
// чтобы УМЕТЬ НЕ РАБОТАТЬ: правильное её поведение — прекратить кормить
// железо и дать устройству перезагрузиться. Поэтому проверяются не «вызовы
// состоялись», а именно решения: кто опоздал, когда кормление прекратилось,
// что записано в регистры, переживающие сброс, и не сбрасывает ли устройство
// само себя во время глубокого сна.
//
// Модель «железа» (platform_watchdog_mock.hpp) не перезагружает хост: она
// считает, сколько раз таймер сработал бы (WatchdogModel::expired), и хранит
// резервные регистры, «переживающие» вызов reset(false).
// ---------------------------------------------------------------------------
#include "os/watchdog/watchdog.hpp"
#include "os/power/power.hpp"

#include "os/config/time_config.hpp"
#include "os/core/memory/memory_manager.hpp"
#include "os/core/task/sheduler.hpp"

#include "platform_watchdog_mock.hpp"   // модель сторожа mock-платформы
#include "platform_power_mock.hpp"      // модель энергосбережения (гранулярность сна)

#include <cstdio>

namespace {

int g_failed = 0;
int g_total  = 0;

void check(const char* what, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    (void)std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

template <typename T>
void check_eq(const char* what, T got, T expected) {
    const bool ok = (got == expected);
    ++g_total;
    if (!ok) ++g_failed;
    (void)std::printf("  [%s] %-54s got=%lld expected=%lld\n",
                      ok ? "OK  " : "FAIL", what,
                      static_cast<long long>(got),
                      static_cast<long long>(expected));
}

using embed::Status;
using embed::scheduler;
using embed::config::OsTimer;
using embed::watchdog::Service;
using embed::watchdog::Participant;
using embed::watchdog::Config;
using embed::watchdog::Mode;
using embed::watchdog::Stats;
using embed::watchdog::Fault;
namespace mock = embed::mock_platform;

Service&    W() { return Service::instance(); }
Stats    wstats() { return Service::instance().stats(); }

// ─────────────────────────── Участники слежения ─────────────────────────────
//
// Три «подсистемы» с разными сроками: быстрый датчик, канал связи и медленная
// задача (её срок определяет аппаратный таймаут — он один на всех).
Participant g_sensor;
Participant g_link;
Participant g_slow;

constexpr uint32_t kSensorTimeout = 500u;
constexpr uint32_t kLinkTimeout   = 1000u;
constexpr uint32_t kSlowTimeout   = 5000u;
constexpr uint32_t kCheckPeriod   = 100u;
constexpr uint32_t kSleepSafety   = kCheckPeriod + 1000u;   // период + гранулярность RTC

/// Прогон времени: тик ОС + продвижение модели «железа» на тот же шаг.
/// Так аппаратный таймер живёт в том же времени, что и система.
void run_ms(uint32_t ms, bool check_all = true, bool check_sensor = true,
            bool check_link = true, bool check_slow = true) {
    for (uint32_t i = 0; i < ms; ++i) {
        if (check_all && (i % 400u) == 0u) {
            if (check_sensor) (void)g_sensor.check_in();
            if (check_link)   (void)g_link.check_in();
            if (check_slow)   (void)g_slow.check_in();
        }
        OsTimer::TickIsr();
        mock::watchdog_model().advance(1u);
    }
}

// ──────────────────────────── Настройка и отказы ────────────────────────────

void test_init() {
    (void)std::puts("инициализация и посмертная диагностика:");

    mock::WatchdogModel& m = mock::watchdog_model();
    m.reset();

    // Прошлая загрузка: сторож сбросил устройство, отметку успели записать.
    // Резервные регистры «переживают» сброс — поэтому заполняем их до init.
    m.notes[embed::watchdog::kNoteIdxMagic]  = embed::watchdog::kNoteMagic;
    m.notes[embed::watchdog::kNoteIdxBoot]   = 7u;
    m.notes[embed::watchdog::kNoteIdxFault]  = 2u;      // участник с индексом 1
    m.notes[embed::watchdog::kNoteIdxLate]   = 250u;    // опоздал на 250 мс
    m.notes[embed::watchdog::kNoteIdxFBoot]  = 7u;      // случилось в 7-й загрузке

    m.reset_cause_flags = static_cast<uint32_t>(embed::platform::ResetCause::IndependentWatchdog);

    Config cfg{};
    cfg.check_period_ms = kCheckPeriod;

    // Сбрасывать нечем — конфигурация «Reset без железа» обязана быть ошибкой,
    // а не тихой работой вхолостую.
    const uint32_t saved_caps = m.caps;
    m.caps = static_cast<uint32_t>(embed::platform::WatchdogCaps::ResetCause);
    check("Mode::Reset без аппаратного таймера -> NotSupported",
          W().init(cfg) == Status::NotSupported);
    check("после отказа сервис не инициализирован", !W().initialized());
    m.caps = saved_caps;

    Config zero{};
    zero.check_period_ms = 0u;
    check("check_period_ms == 0 -> InvalidArg", W().init(zero) == Status::InvalidArg);

    // Настоящая инициализация.
    check("init() == Ok", W().init(cfg) == Status::Ok);
    check("повторный init() -> AlreadyExists", W().init(cfg) == Status::AlreadyExists);

    // Причина сброса прочитана РОВНО один раз: чтение RCC_CSR сбрасывает флаги.
    check_eq<unsigned long long>("причина сброса прочитана один раз", m.cause_reads, 1ull);
    check_eq<unsigned long long>("флаги причины сброса сняты чтением",
          m.reset_cause_flags, 0ull);
    check("причина = независимый сторожевой таймер",
          (W().reset_cause() &
           static_cast<uint32_t>(embed::platform::ResetCause::IndependentWatchdog)) != 0u);

    const Fault f = W().last_fault();
    check("посмертная отметка распознана", f.valid);
    check_eq<unsigned long long>("участник из отметки", f.participant, 1ull);
    check_eq<unsigned long long>("опоздание из отметки", f.late_ms, 250ull);
    check_eq<unsigned long long>("счётчик загрузок увеличен", f.boot_count, 8ull);

    // Отметка из ПОЗАПРОШЛОЙ загрузки не должна объяснять текущий запуск.
    m.notes[embed::watchdog::kNoteIdxFBoot] = 3u;
    // Причина сброса (сторож) всё ещё известна из RCC_CSR, поэтому запись
    // остаётся достоверной, но участника приписать текущей загрузке нельзя.
    const Fault stale = W().last_fault();
    check("устаревшая отметка не приписывается этой загрузке",
          stale.participant == embed::watchdog::kNoParticipant);
    m.notes[embed::watchdog::kNoteIdxFBoot] = 7u;
    check("свежая отметка снова видна", W().last_fault().participant == 1u);

    check("clear_fault() == Ok", W().clear_fault() == Status::Ok);
    check_eq<unsigned long long>("отметка стёрта в регистрах",
          m.notes[embed::watchdog::kNoteIdxFault], 0ull);
    check("после clear_fault() участник неизвестен",
          W().last_fault().participant == embed::watchdog::kNoParticipant);

    // Сброс от сторожа БЕЗ отметки (например, не работало прерывание тика)
    // обязан быть виден: молчать о таком нельзя.
    m.reset_cause_flags = static_cast<uint32_t>(embed::platform::ResetCause::IndependentWatchdog);
    const Fault f2 = W().last_fault();
    check("сброс от сторожа без отметки распознан", f2.valid);
    check_eq<unsigned long long>("участник неизвестен", f2.participant,
          static_cast<unsigned long long>(embed::watchdog::kNoParticipant));
}

// ──────────────────────────── Участники и запуск ────────────────────────────

void test_participants_and_start() {
    (void)std::puts("участники и запуск:");

    mock::WatchdogModel& m = mock::watchdog_model();

    check("участник без имени -> InvalidArg",
          g_sensor.init(nullptr, kSensorTimeout) == Status::InvalidArg);
    check("срок 0 -> InvalidArg",
          g_sensor.init("sensor", 0u) == Status::InvalidArg);
    check("min_interval >= срока -> InvalidArg",
          g_sensor.init("sensor", 100u, 100u) == Status::InvalidArg);

    check("датчик зарегистрирован",
          g_sensor.init("sensor", kSensorTimeout, 300u) == Status::Ok);
    check("повторная регистрация -> AlreadyExists",
          g_sensor.init("sensor", kSensorTimeout) == Status::AlreadyExists);
    check("канал связи зарегистрирован",
          g_link.init("link", kLinkTimeout) == Status::Ok);
    check("медленная задача зарегистрирована",
          g_slow.init("slow", kSlowTimeout) == Status::Ok);

    // Отметка незарегистрированного участника — ошибка, а не «молча ничего».
    Participant stray;
    check("отметка незарегистрированного -> NotFound", stray.check_in() == Status::NotFound);

    check_eq<unsigned long long>("сервис видит трёх участников",
          wstats().participants, 3ull);

    // ── Запуск ──
    check("start() == Ok", W().start() == Status::Ok);
    check("start() дважды -> AlreadyExists", W().start() == Status::AlreadyExists);
    check("аппаратный таймер запущен", m.started);
    check_eq<unsigned long long>("первое кормление сразу при старте", m.kick_calls, 1ull);

    // Аппаратный таймаут = максимальный срок участника + запас, а запас обязан
    // вмещать период проверки и гранулярность пробуждения из сна.
    const uint32_t min_margin = 2u * kCheckPeriod + kSleepSafety;
    const uint32_t expected_hw = ((kSlowTimeout + min_margin + m.timeout_step_ms - 1u)
                                 / m.timeout_step_ms) * m.timeout_step_ms;
    check_eq<unsigned long long>("аппаратный таймаут с запасом", m.timeout_ms, expected_hw);
    check_eq<unsigned long long>("сервис сообщает фактический таймаут",
          W().hardware_timeout_ms(), m.timeout_ms);

    // ── Регистрация после старта ──
    Participant late_task;
    check("регистрация после start() -> Busy",
          late_task.init("late", 100u) == Status::Busy);

    check("остановка независимого таймера невозможна (IWDG)",
          W().stop() == Status::NotSupported);
}

// ─────────────────────────── Кормление по расписанию ────────────────────────

void test_kick_cadence() {
    (void)std::puts("кормление по расписанию:");

    mock::WatchdogModel& m = mock::watchdog_model();
    const uint32_t kicks0 = m.kick_calls;

    // Полный период: кормления ещё не было (оно было при старте).
    run_ms(kCheckPeriod - 1u);
    check_eq<unsigned long long>("до периода кормления нет",
          m.kick_calls - kicks0, 0ull);

    // Ещё 1 мс — кормление состоялось.
    run_ms(1u);
    check_eq<unsigned long long>("на периоде — кормление", m.kick_calls - kicks0, 1ull);

    // 1000 мс живых участников: 10 кормлений, ни одной просрочки.
    const uint32_t kicks1 = m.kick_calls;
    run_ms(1000u);
    check_eq<unsigned long long>("за 1000 мс — 10 кормлений",
          m.kick_calls - kicks1, 10ull);
    check_eq<unsigned long long>("просрочек нет", wstats().misses, 0ull);
    check_eq<unsigned long long>("железо не срабатывало", m.expired, 0ull);

    check("отметки участников учтены", g_sensor.checks() > 0u);
    check_eq<unsigned long long>("участник не пропустил отметок", g_sensor.misses(), 0ull);
}

// ────────────────────────── Интеграция с глубоким сном ──────────────────────

void test_deep_sleep_cooperation() {
    (void)std::puts("согласование с глубоким сном:");

    mock::WatchdogModel& m = mock::watchdog_model();
    mock::PowerModel& pm = mock::power_model();

    // Ограничение сна: аппаратный таймаут минус запас на пробуждение.
    const uint32_t limit = W().sleep_limit_ms();
    check_eq<unsigned long long>("сон ограничен сроком сторожа", limit,
          m.timeout_ms - kSleepSafety);

    // Супервизор энергосбережения обязан УВИДЕТЬ это ограничение: иначе
    // устройство уснёт дольше срока сторожа и сбросит себя само.
    const uint32_t horizon = embed::power::Supervisor::instance().horizon_ms();
    check("горизонт сна супервизора не больше ограничения сторожа", horizon <= limit);
    check_eq<unsigned long long>("горизонт равен ограничению сторожа", horizon, limit);

    // ── Сон ──
    // Платформа обещает проспать 4 с; сторож разрешает меньше, значит спать
    // будем ровно столько, сколько разрешено.
    pm.deep_horizon_ms = 4000u;
    const uint32_t kicks_before = m.kick_calls;
    const uint32_t slept_before = wstats().slept_ms;

    embed::power::Supervisor::instance().on_idle();

    const Stats s = wstats();
    check_eq<unsigned long long>("сон под сторожем засчитан", s.deep_sleeps, 1ull);
    check("сон длился не меньше запрошенного", s.slept_ms - slept_before > 0u);
    check("после сна сторож получил кормление", m.kick_calls > kicks_before);

    // Сроки участников после сна начинаются заново: во сне отметиться было
    // некому, и без пересчёта первый же тик обнаружил бы просрочку у всех.
    check_eq<unsigned long long>("датчик не «опоздал» из-за сна", g_sensor.misses(), 0ull);
    check_eq<unsigned long long>("просрочек нет", s.misses, 0ull);
    check("возраст отметки отсчитан от пробуждения", g_sensor.age_ms() < kSensorTimeout);

    // Железо: за время сна таймер не сработал (сон уложился в его срок).
    mock::watchdog_model().advance(s.slept_ms - slept_before);
    check_eq<unsigned long long>("за время сна сторож не сработал", m.expired, 0ull);

    // Живая работа после сна продолжается как обычно.
    const uint32_t kicks2 = m.kick_calls;
    run_ms(500u);
    check("после сна кормление продолжается", m.kick_calls > kicks2);
    check_eq<unsigned long long>("и просрочек по-прежнему нет", wstats().misses, 0ull);
}

// ──────────────────────── Оконный сторож и длительные операции ──────────────

void test_early_checks_and_stalls() {
    (void)std::puts("ранняя отметка и длительные операции:");

    mock::WatchdogModel& m = mock::watchdog_model();
    const Stats before = wstats();

    // Отметка раньше min_interval_ms — признак «задача крутится, но работу не
    // делает». Это программная замена оконному сторожу (WWDG).
    //
    // Время приводим в ИЗВЕСТНОЕ состояние: отметка (возраст любой), затем
    // 350 мс без отметок — это больше min_interval (300) и меньше срока
    // датчика (500), поэтому следующая отметка обязана быть принята, а
    // следующая за ней (возраст 0) — обязана быть нарушением.
    (void)g_sensor.check_in();
    const uint32_t early_before = wstats().early_checks;
    run_ms(350u, /*check_all*/ false);

    check("отметка в срок", g_sensor.check_in() == Status::Ok);
    check("ранняя отметка не нарушение",
          wstats().early_checks == early_before);
    check("слишком ранняя отметка распознана",
          g_sensor.check_in() == Status::AlreadyExists);
    check_eq<unsigned long long>("нарушение посчитано ровно одно",
          wstats().early_checks - early_before, 1ull);
    check("но просрочкой это не стало", wstats().misses == before.misses);

    // Длительная операция: окно ограничено конфигурацией (max_stall_ms = 0).
    check("длительные операции запрещены без предела в конфигурации",
          W().allow_stall(1000u) == Status::NotSupported);
    check_eq<unsigned long long>("отказ посчитан",
          wstats().refused_stalls - before.refused_stalls, 1ull);

    // …а с нулевым окном и подавно.
    check("окно 0 мс отклонено", W().allow_stall(0u) == Status::NotSupported);

    // Пока окна нет, сон разрешён (ограничение работает).
    check("сон не запрещён вне окна", W().sleep_limit_ms() > 0u);
    check_eq<unsigned long long>("остаток окна нулевой", W().stall_left_ms(), 0ull);

    // За время этих проверок кормление шло обычным порядком.
    check("кормление не прекращалось", m.kick_calls > 0u);
}

// ───────────────────── Просрочка: решение и посмертная запись ───────────────

void test_miss_by_timeout() {
    (void)std::puts("просрочка по сроку:");

    mock::WatchdogModel& m = mock::watchdog_model();
    const Stats before = wstats();

    // Датчик перестаёт отмечаться: остальные работают.
    // Кормления продолжаются, пока просрочка не обнаружена, — это и есть
    // «окно реакции» сторожа.
    run_ms(kSensorTimeout + 3u * kCheckPeriod, /*check_all*/ true, /*sensor*/ false);

    const Stats s = wstats();
    check("просрочка обнаружена", s.misses > before.misses);
    check("участник знает о своих пропусках", g_sensor.misses() > 0u);
    check("предупреждение выдано один раз", s.warnings - before.warnings == 1u);

    // Посмертная отметка: кто и насколько опоздал (читается следующей
    // загрузкой, но записывается здесь).
    check_eq<unsigned long long>("в отметке — индекс провинившегося",
          m.notes[embed::watchdog::kNoteIdxFault], 1ull);      // sensor = индекс 0 → +1
    check("в отметке есть опоздание", m.notes[embed::watchdog::kNoteIdxLate] > 0u);
    check_eq<unsigned long long>("отметка помечена текущей загрузкой",
          m.notes[embed::watchdog::kNoteIdxFBoot], W().last_fault().boot_count);

    // Главное: кормление ПРЕКРАТИЛОСЬ. Дальше устройство сбросит железо —
    // ровно для этого сторож и существует.
    const uint32_t kicks_at_miss = m.kick_calls;
    run_ms(m.timeout_ms + kCheckPeriod);   // время сторожа истекло без кормления
    check_eq<unsigned long long>("после просрочки железо не кормили ни разу",
          m.kick_calls, kicks_at_miss);
    check_eq<unsigned long long>("таймер сработал (устройство перезагрузилось)",
          m.expired, 1ull);

    // Защёлка: даже если участник «ожил», решение не отменяется само —
    // иначе сторож маскировал бы растущую проблему.
    const uint32_t kicks_after_reset = m.kick_calls;
    m.started = true;          // «железо» снова считает после сброса
    g_sensor.check_in();
    run_ms(3u * kCheckPeriod);
    check_eq<unsigned long long>("оживший участник не отменяет решение",
          m.kick_calls, kicks_after_reset);

    // Осознанное снятие защёлки приложением: сервис снова кормит и считает
    // сроки заново.
    check("rearm() == Ok", W().rearm() == Status::Ok);
    check_eq<unsigned long long>("снятие защёлки посчитано", wstats().rearms, 1ull);
    const uint32_t kicks_rearmed = m.kick_calls;
    run_ms(2u * kCheckPeriod);
    check("после rearm() кормление возобновилось", m.kick_calls > kicks_rearmed);
    check_eq<unsigned long long>("rearm() без защёлки -> AlreadyExists",
          static_cast<unsigned long long>(W().rearm()),
          static_cast<unsigned long long>(Status::AlreadyExists));
}

// ─────────────────────── Явный отказ участника (fail) ───────────────────────

void test_miss_by_fail() {
    (void)std::puts("явный отказ участника:");

    mock::WatchdogModel& m = mock::watchdog_model();
    const Stats before = wstats();

    // Задача сообщает о неустранимой ошибке, не дожидаясь своего срока.
    g_link.fail();
    run_ms(kCheckPeriod + 1u, /*check_all*/ true);
    const uint32_t kicked = m.kick_calls;

    check("отказ участника обнаружен", wstats().misses > before.misses);
    check_eq<unsigned long long>("в отметке — отказавший участник",
          m.notes[embed::watchdog::kNoteIdxFault], 2ull);      // link = индекс 1 → +1
    check_eq<unsigned long long>("отказавший участник назван", g_link.failed(), 1ull);

    run_ms(m.timeout_ms + kCheckPeriod);
    check_eq<unsigned long long>("после отказа кормления не было",
          m.kick_calls, kicked);
    check_eq<unsigned long long>("таймер сработал", m.expired, 2ull);

    // Приложение нашло и перезапустило отвалившуюся подсистему: защёлка
    // снимается вместе с признаком отказа участника.
    check("rearm() после отказа участника == Ok", W().rearm() == Status::Ok);
    check("признак отказа снят", !g_link.failed());
    const uint32_t kicks_rearmed = m.kick_calls;
    run_ms(2u * kCheckPeriod);
    check("после rearm() кормление возобновилось", m.kick_calls > kicks_rearmed);
    check_eq<unsigned long long>("снятий защёлки — два", wstats().rearms, 2ull);
}

// ──────────────────────────────── Итоги ─────────────────────────────────────

void test_summary() {
    (void)std::puts("итоговая статистика:");

    const Stats s = wstats();
    check_eq<unsigned long long>("участников зарегистрировано", s.participants, 3ull);
    check_eq<unsigned long long>("аппаратный таймер — да", s.hardware ? 1ull : 0ull, 1ull);
    check("проверок сроков было много", s.checks > 0u);
    check("кормлений было много", s.kicks > 0u);
    check("максимальное опоздание зафиксировано", s.max_late_ms > 0u);
    check("снятий защёлки — два", s.rearms == 2u);
    check("статическое потребление RAM > 0", Service::static_bytes() > 0u);
    check("участник тоже занимает RAM", Participant::static_bytes() > 0u);
}

} // namespace

int main() {
    using namespace embed;

    (void)std::puts("=== тесты сторожевого таймера ===");

    if (g_memory_manager.init(4096u, 512u) != Status::Ok) {
        (void)std::puts("FAIL: memory init");
        return 1;
    }

    // ── Время ──
    OsTimer::Config tcfg{};
    tcfg.min_tick_ms     = 1;
    tcfg.max_tick_ms     = 1000;
    tcfg.default_tick_ms = 1;
    if (OsTimer::Init(tcfg) != Status::Ok) {
        (void)std::puts("FAIL: timer init");
        return 1;
    }

    // ── Супервизор энергосбережения: нужен, чтобы проверить согласование
    //    ограничения сна (и чтобы горизонт сна учитывал сторожа) ──
    power::Config pcfg{};
    pcfg.allow_deep_sleep = true;
    pcfg.window_ms        = 1000u;
    if (power::Supervisor::instance().init(pcfg) != Status::Ok) {
        (void)std::puts("FAIL: power supervisor init");
        return 1;
    }

    // ── Хуки: супервизор энергосбережения первым (он смотрит на момент
    //    прерывания), затем сторож, затем планировщик ──
    if (OsTimer::RegisterSystemHook(&power::Supervisor::tick_hook, nullptr) != Status::Ok ||
        OsTimer::RegisterSystemHook(&Service::tick_hook, nullptr) != Status::Ok ||
        OsTimer::RegisterSystemHook(&scheduler::tick_hook, nullptr) != Status::Ok) {
        (void)std::puts("FAIL: hook registration");
        return 1;
    }
    if (OsTimer::Start() != Status::Ok) {
        (void)std::puts("FAIL: timer start");
        return 1;
    }

    test_init();
    test_participants_and_start();
    test_kick_cadence();
    test_deep_sleep_cooperation();
    test_early_checks_and_stalls();
    test_miss_by_timeout();
    test_miss_by_fail();
    test_summary();

    (void)std::printf("\nвсего проверок: %d, провалено: %d\n", g_total, g_failed);
    if (g_failed != 0) (void)std::puts("FAIL");
    return (g_failed == 0) ? 0 : 1;
}
