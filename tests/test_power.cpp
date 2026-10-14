// tests/test_power.cpp
// ---------------------------------------------------------------------------
// Супервизор энергосбережения: учёт нагрузки, горизонт сна, допуск глубокого
// сна, корректировка времени после STOP и профили частоты.
//
// ПОЧЕМУ НА МОДЕЛЬНОЙ ПЛАТФОРМЕ
// Цена ошибки в этой подсистеме — не «медленно», а «уснул и не проснулся»:
// в STOP гаснет тактирование, и незавершённый обмен или неразбуженный таймер
// не лечатся ничем, кроме Reset. Модель (platform_power_mock.hpp) позволяет
// воспроизвести все такие состояния явно: источника пробуждения нет,
// будильник грубый, платформа отказала во входе, сон оказался длиннее
// запрошенного. Каждое решение супервизора здесь проверяется числом.
// ---------------------------------------------------------------------------
#include "os/power/power.hpp"

#include "os/config/time_config.hpp"
#include "os/core/memory/memory_manager.hpp"
#include "os/core/task/sheduler.hpp"

#include "platform_power_mock.hpp"   // модель энергосбережения mock-платформы

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
using embed::task;
using embed::config::OsTimer;
using embed::power::Supervisor;
using embed::power::Config;
using embed::power::Profile;
using embed::power::Stats;
namespace mock = embed::mock_platform;

Supervisor& P0() { return Supervisor::instance(); }
Stats stats()     { return Supervisor::instance().stats(); }

task     g_busy;
uint32_t g_busy_stack[48];

void tick(uint32_t times) {
    for (uint32_t i = 0; i < times; ++i) OsTimer::TickIsr();
}

// ─────────────────────────── Арифметика частоты ─────────────────────────────

void test_project_load() {
    (void)std::puts("проекция нагрузки на другую частоту:");

    // Нагрузка обратно пропорциональна частоте: 50 % на 72 МГц — это 450 %
    // на 8 МГц. Это и есть инструмент выбора СТАТИЧЕСКОГО профиля: цифра
    // >100 означает «на этой частоте система не успеет», какой бы
    // экономичной она ни выглядела.
    check_eq<unsigned long long>(
        "50 % на 72 МГц -> 450 % на 8 МГц",
        embed::power::project_load_percent(50u, 72000000u, 8000000u), 450ull);
    check_eq<unsigned long long>(
        "10 % на 72 МГц -> 90 % на 8 МГц",
        embed::power::project_load_percent(10u, 72000000u, 8000000u), 90ull);
    check_eq<unsigned long long>(
        "100 % на 72 МГц -> 900 % на 8 МГц",
        embed::power::project_load_percent(100u, 72000000u, 8000000u), 900ull);
    check_eq<unsigned long long>(
        "обрезка на 1000 %",
        embed::power::project_load_percent(100u, 72000000u, 1000000u), 1000ull);
    check_eq<unsigned long long>(
        "нулевая нагрузка остаётся нулевой",
        embed::power::project_load_percent(0u, 72000000u, 8000000u), 0ull);
    check_eq<unsigned long long>(
        "неизвестная частота — вход без изменений",
        embed::power::project_load_percent(40u, 0u, 8000000u), 40ull);
}

// ─────────────────────────────── Настройка ─────────────────────────────────

void test_init_and_profile() {
    (void)std::puts("инициализация и профиль частоты:");

    mock::power_model() = mock::PowerModel{};   // чистая модель «железа»

    Config cfg{};
    cfg.window_ms = 1000u;

    check("init() == Ok", P0().init(cfg) == Status::Ok);
    check("initialized()", P0().initialized());
    check_eq<unsigned long long>("профиль по умолчанию — Performance",
          static_cast<unsigned long long>(P0().profile()),
          static_cast<unsigned long long>(Profile::Performance));
    check_eq<unsigned long long>("частота взята у платформы",
          P0().sysclk_hz(), 72000000ull);
    check_eq<unsigned long long>(
        "частоту при совпадении профиля НЕ переключали",
        mock::power_model().profile_calls, 0ull);
    check("повторный init() -> AlreadyExists",
          P0().init(cfg) == Status::AlreadyExists);
    check("configure() на инициализированном == Ok",
          P0().configure(cfg) == Status::Ok);

    // DeepSleep, которого у платформы нет, обязан быть ошибкой конфигурации,
    // а не тихим «мы не спим» — иначе отчёт введёт в заблуждение.
    mock::PowerModel saved = mock::power_model();
    mock::power_model().caps = static_cast<uint32_t>(embed::platform::PowerCaps::Sleep);
    Config deep_cfg = cfg;
    deep_cfg.allow_deep_sleep = true;
    check("allow_deep_sleep без поддержки платформы -> NotSupported",
          P0().configure(deep_cfg) == Status::NotSupported);
    mock::power_model() = saved;

    // LowPower применяется до старта тика.
    Config lp = cfg;
    lp.profile = Profile::LowPower;
    check("configure(LowPower) до старта тика == Ok",
          P0().configure(lp) == Status::Ok);
    check_eq<unsigned long long>("частота стала 8 МГц",
          P0().sysclk_hz(), 8000000ull);
    check_eq<unsigned long long>("профиль LowPower",
          static_cast<unsigned long long>(P0().profile()),
          static_cast<unsigned long long>(Profile::LowPower));

    // Обратно в Performance: mock это позволяет (на STM32 LowPower — выбор
    // на всю загрузку, там set_profile вернул бы 0).
    check("возврат в Performance == Ok", P0().configure(cfg) == Status::Ok);
    check_eq<unsigned long long>("частота снова 72 МГц",
          P0().sysclk_hz(), 72000000ull);
}

// ──────────────────────────── Учёт нагрузки ────────────────────────────────

void test_load_accounting() {
    (void)std::puts("учёт нагрузки:");

    scheduler& s = scheduler::instance();
    const Status st = s.init();
    check("scheduler init", st == Status::Ok || st == Status::AlreadyExists);

    OsTimer::Config tcfg{};
    tcfg.min_tick_ms     = 1;
    tcfg.max_tick_ms     = 1000;
    tcfg.default_tick_ms = 1;
    check("OsTimer::Init == Ok", OsTimer::Init(tcfg) == Status::Ok);

    // Порядок регистрации важен: супервизор должен увидеть, ЧТО исполнялось
    // в момент прерывания, ДО того как планировщик в этом же тике сменит
    // задачу. Поэтому его хук — первым.
    check("hook(power) == Ok",
          OsTimer::RegisterSystemHook(&Supervisor::tick_hook, nullptr) == Status::Ok);
    check("hook(scheduler) == Ok",
          OsTimer::RegisterSystemHook(&scheduler::tick_hook, nullptr) == Status::Ok);
    check("OsTimer::Start == Ok", OsTimer::Start() == Status::Ok);
    check_eq<unsigned long long>("тик = 1 мс", OsTimer::TickMs(), 1ull);

    // Без start() планировщик не обрабатывает тики (tick() сразу выходит), и
    // спящие задачи не просыпаются — на это и был пойман первый прогон.
    check("scheduler::start == Ok", s.start() == Status::Ok);
    check("исполняется idle-задача после start()", s.idle_running());

    // ── Окно 1: 400 мс простоя, затем 600 мс работы ──
    tick(400u);

    auto body = [](void*) noexcept {};
    check("задача создана",
          g_busy.init(body, nullptr, "busy", 48u, /*priority*/ 1u) == Status::Ok);
    check("задача добавлена", s.add_task(&g_busy, g_busy_stack, 48u));
    s.context_switch();
    check("исполняется задача, а не idle", !s.idle_running());
    tick(600u);

    Stats s1 = stats();
    check_eq<unsigned long long>("окно закрыто", s1.windows, 1ull);
    check_eq<unsigned long long>("загрузка окна, %", s1.load_percent, 60ull);
    check_eq<unsigned long long>("пик, %", s1.peak_load_percent, 60ull);
    check_eq<unsigned long long>("средняя, %", s1.avg_load_percent, 60ull);

    // ── Окно 2: 200 мс простоя, 800 мс работы ──
    // Задача сама уходит в сон на 200 тиков: пока она спит, ядро занимает
    // idle. Просыпается она ровно на 200-м тике — это и есть проверка, что
    // «простой» считается по фактическому исполнению, а не по календарю.
    check("задача уснула на 200 тиков", s.sleep_for(200u) == Status::Ok);
    s.context_switch();
    check("пока задача спит, исполняется idle", s.idle_running());
    tick(200u);
    check("задача проснулась", s.sleep_count() == 0u);
    s.context_switch();
    check("задача снова на ядре", !s.idle_running());
    tick(800u);

    Stats s2 = stats();
    check_eq<unsigned long long>("окон стало 2", s2.windows, 2ull);
    check_eq<unsigned long long>("загрузка второго окна, %", s2.load_percent, 80ull);
    check_eq<unsigned long long>("пик обновлён, %", s2.peak_load_percent, 80ull);
    check_eq<unsigned long long>("средняя по двум окнам, %", s2.avg_load_percent, 70ull);
    check_eq<unsigned long long>("простой второго окна, мс", s2.idle_ms, 0ull);
    check("текущее окно начато заново", s2.window_ms < 1000u);
}

// ─────────────────────────── Горизонт сна ──────────────────────────────────

void test_horizon() {
    (void)std::puts("горизонт сна (сколько можно спать):");

    scheduler& s = scheduler::instance();

    check_eq<unsigned long long>("без сроков — ограничения нет",
          P0().horizon_ms(), embed::kNoDeadline);

    P0().set_wake_hint_ms(250u);
    check_eq<unsigned long long>("подсказка приложения ограничивает горизонт",
          P0().horizon_ms(), 250ull);
    P0().set_wake_hint_ms(0u);
    check_eq<unsigned long long>("подсказка снята",
          P0().horizon_ms(), embed::kNoDeadline);

    // Спящая задача: просыпаться раньше её срока нельзя.
    check("задача уснула на 5 тиков", s.sleep_for(5u) == Status::Ok);
    check_eq<unsigned long long>("горизонт = срок спящей задачи (5 тиков × 1 мс)",
          P0().horizon_ms(), 5ull);
    tick(5u);
    check("задача проснулась", s.sleep_count() == 0u);

    // Программный таймер ОС: его срок тоже граница сна.
    OsTimer::TimerHandle th{};
    auto cb = [](void*) noexcept {};
    check("таймер создан",
          OsTimer::CreateTimer(100u, false, cb, nullptr, th) == Status::Ok);
    check("таймер запущен", OsTimer::StartTimer(th) == Status::Ok);
    check_eq<unsigned long long>("ближайший срок таймера, мс",
          OsTimer::NextDeadlineMs(), 100ull);
    check_eq<unsigned long long>("горизонт ограничен таймером",
          P0().horizon_ms(), 100ull);
    check("таймер удалён", OsTimer::RemoveTimer(th) == Status::Ok);
    check_eq<unsigned long long>("после удаления таймера сроков нет",
          P0().horizon_ms(), embed::kNoDeadline);
    check_eq<unsigned long long>("тик вернулся к 1 мс",
          OsTimer::TickMs(), 1ull);
}

// ───────────────────────── Допуск глубокого сна ────────────────────────────

void test_deep_sleep_policy() {
    (void)std::puts("допуск глубокого сна:");

    scheduler& s = scheduler::instance();
    mock::PowerModel& m = mock::power_model();

    Config cfg{};
    cfg.window_ms          = 1000u;
    cfg.allow_deep_sleep   = true;
    cfg.deep_sleep_min_ms  = 8u;

    // ── Источник пробуждения не вооружён ──
    m.deep_horizon_ms = 0u;
    check("configure(deep) == Ok", P0().configure(cfg) == Status::Ok);

    const Stats before = stats();
    uint32_t idle_before = m.idle_calls;
    P0().on_idle();
    check_eq<unsigned long long>("входов в Sleep (WFI) прибавилось",
          m.idle_calls - idle_before, 1ull);
    check_eq<unsigned long long>("отказ: нет источника пробуждения",
          stats().refused_no_source - before.refused_no_source, 1ull);
    check_eq<unsigned long long>("в DeepSleep не входили",
          m.deep_calls, 0ull);

    // ── Будильник есть, но горизонт короче порога окупаемости ──
    m.deep_horizon_ms     = 5000u;
    m.deep_granularity_ms = 1000u;
    P0().set_wake_hint_ms(500u);
    check_eq<unsigned long long>("горизонт = подсказка (500 мс)",
          P0().horizon_ms(), 500ull);
    P0().on_idle();
    check_eq<unsigned long long>("отказ: горизонт короче порога + гранулярность",
          stats().refused_short - before.refused_short, 1ull);

    // ── Систему удерживает незавершённый обмен ──
    P0().set_wake_hint_ms(0u);
    {
        auto lease = P0().keep_awake();
        check("удержание взято", lease.held());
        check_eq<unsigned long long>("счётчик удержаний", P0().keep_awake_count(), 1ull);
        P0().on_idle();
        check_eq<unsigned long long>("отказ: систему удерживают",
              stats().refused_keep_awake - before.refused_keep_awake, 1ull);
        check_eq<unsigned long long>("в DeepSleep всё ещё не входили",
              m.deep_calls, 0ull);
    }
    check_eq<unsigned long long>("удержание отпущено",
          P0().keep_awake_count(), 0ull);

    // ── Никто не мешает: уходим в глубокий сон ──
    const uint64_t now_before = OsTimer::NowMs();
    const uint64_t ticks_before = s.tick_count();
    const uint32_t deep_before = stats().deep_sleep_entries;
    P0().on_idle();
    check_eq<unsigned long long>("попытка входа", m.deep_calls, 1ull);
    check_eq<unsigned long long>("запрошен весь гарантированный горизонт",
          m.last_request_ms, 5000ull);
    check_eq<unsigned long long>("вошли в DeepSleep",
          stats().deep_sleep_entries - deep_before, 1ull);
    check_eq<unsigned long long>("проспано, мс", stats().deep_sleep_ms, 5000ull);
    check_eq<unsigned long long>("режим последнего простоя — DeepSleep",
          static_cast<unsigned long long>(P0().last_mode()),
          static_cast<unsigned long long>(embed::power::Mode::DeepSleep));
    check_eq<unsigned long long>("монотонное время ОС продвинуто на сон",
          OsTimer::NowMs() - now_before, 5000ull);
    check_eq<unsigned long long>("тики планировщика «догнали» сон",
          s.tick_count() - ticks_before, 5000ull);

    // ── Платформа отказала во входе ──
    m.deep_enter = false;
    uint32_t idle2 = m.idle_calls;
    P0().on_idle();
    check_eq<unsigned long long>("отказ платформы учтён",
          stats().deep_sleep_failed, 1ull);
    check_eq<unsigned long long>("после отказа всё равно ушли в Sleep",
          m.idle_calls - idle2, 1ull);
    m.deep_enter = true;
}

// ───────────── Время сна и спящие задачи (главная проверка) ────────────────

void test_deep_sleep_time_accounting() {
    (void)std::puts("сон и сроки задач:");

    scheduler& s = scheduler::instance();
    mock::PowerModel& m = mock::power_model();

    // ── Задача, срок которой истекает ВНУТРИ сна ──
    // Горизонт берётся по её сроку: спать дольше — проспать задачу.
    P0().set_wake_hint_ms(5000u);
    m.deep_horizon_ms = 5000u;
    s.context_switch();                       // задача снова на ядре
    check("задача уснула на 3000 тиков", s.sleep_for(3000u) == Status::Ok);
    check_eq<unsigned long long>("горизонт ограничен сроком задачи",
          P0().horizon_ms(), 3000ull);

    const uint32_t deep_before = stats().deep_sleep_entries;
    P0().on_idle();
    check_eq<unsigned long long>("спали по сроку задачи",
          m.last_request_ms, 3000ull);
    check_eq<unsigned long long>("вход в DeepSleep засчитан",
          stats().deep_sleep_entries - deep_before, 1ull);
    check_eq<unsigned long long>("задача разбужена сном (не потеряна)",
          s.sleep_count(), 0ull);
    check("задача стала готовой", g_busy.is_ready());

    // ── Задача, чей срок НЕ истёк внутри сна ──
    // Здесь проверяется именно «догон» задержек: без него задача ждала бы
    // ещё столько же тиков, сколько проспала система.
    P0().set_wake_hint_ms(4000u);
    s.context_switch();
    check("задача уснула на 5000 тиков", s.sleep_for(5000u) == Status::Ok);
    // На реальном МКУ спящая задача немедленно уступает ядро (yield внутри
    // sleep_for); на хосте переключения нет, поэтому делаем его явно — иначе
    // последующие тики записались бы в «занято» и статистика соврала бы.
    s.context_switch();
    P0().on_idle();
    check_eq<unsigned long long>("спали по подсказке", m.last_request_ms, 4000ull);
    check_eq<unsigned long long>("остаток задержки задачи, тиков",
          g_busy.delay_ticks(), 1000ull);
    check_eq<unsigned long long>("задача всё ещё в sleep_list",
          s.sleep_count(), 1ull);

    // Досыпаем остаток обычными тиками — задача обязана проснуться ровно
    // через 1000 тиков, а не через 5000.
    tick(1000u);
    check_eq<unsigned long long>("доспала ровно остаток", s.sleep_count(), 0ull);

    // ── Сон оказался длиннее запрошенного (гранулярность будильника) ──
    // RTC на STM32F1 будит с точностью до секунды, поэтому платформа вправе
    // вернуть больше, чем просили. Время ОС обязано продвинуться на
    // ИЗМЕРЕННОЕ значение, иначе таймеры уедут назад.
    P0().set_wake_hint_ms(6000u);
    m.deep_slept_ms = 7000u;
    const uint64_t now_before = OsTimer::NowMs();
    P0().on_idle();
    check_eq<unsigned long long>("платформа проспала больше запрошенного",
          m.last_slept_ms, 7000ull);
    check_eq<unsigned long long>("время ОС продвинуто по факту",
          OsTimer::NowMs() - now_before, 7000ull);
    m.deep_slept_ms = 0u;
    P0().set_wake_hint_ms(0u);

    // ── Остановленный таймер: продвигать время нечем ──
    check("OsTimer::Stop == Ok", OsTimer::Stop() == Status::Ok);
    check("AdvanceMs при остановленном таймере -> Unavailable",
          OsTimer::AdvanceMs(10u) == Status::Unavailable);
    check("AdvanceMs(0) == Ok", OsTimer::AdvanceMs(0u) == Status::Ok);
}

// ─────────────────────────────── Статистика ────────────────────────────────

void test_stats_summary() {
    (void)std::puts("итоговая статистика:");

    const Stats s = stats();
    // Значения по конкретным окнам проверены в тесте нагрузки; здесь важно,
    // что счётчики не «поехали» и что отказы посчитаны по причинам.
    check("окон измерения не меньше двух", s.windows >= 2u);
    check_eq<unsigned long long>("пик загрузки, % (второе окно)", s.peak_load_percent, 80ull);
    check_eq<unsigned long long>("входов в Sleep > 0", s.sleep_entries > 0u, 1ull);
    check_eq<unsigned long long>("входов в DeepSleep", s.deep_sleep_entries, 4ull);
    check_eq<unsigned long long>("суммарно проспано, мс", s.deep_sleep_ms, 19000ull);
    check_eq<unsigned long long>("отказ: нет источника", s.refused_no_source, 1ull);
    check_eq<unsigned long long>("отказ: короткий горизонт", s.refused_short, 1ull);
    check_eq<unsigned long long>("отказ: удержание", s.refused_keep_awake, 1ull);
    check_eq<unsigned long long>("отказ платформы", s.deep_sleep_failed, 1ull);
    check("статическое потребление RAM супервизором > 0",
          Supervisor::static_bytes() > 0u);
}

} // namespace

int main() {
    using namespace embed;

    (void)std::puts("=== тесты супервизора энергосбережения ===");

    if (g_memory_manager.init(4096u, 512u) != Status::Ok) {
        (void)std::puts("FAIL: memory init");
        return 1;
    }

    test_project_load();
    test_init_and_profile();
    test_load_accounting();
    test_horizon();
    test_deep_sleep_policy();
    test_deep_sleep_time_accounting();
    test_stats_summary();

    (void)std::printf("\nвсего проверок: %d, провалено: %d\n", g_total, g_failed);
    if (g_failed != 0) (void)std::puts("FAIL");
    return (g_failed == 0) ? 0 : 1;
}
