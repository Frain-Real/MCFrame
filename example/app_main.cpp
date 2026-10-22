// example/app_main.cpp
// ---------------------------------------------------------------------------
// Точка входа приложения. Порядок инициализации ядра ОС.
//
// Прежний main.cpp лежал в библиотеке (src/os/main.cpp) и при этом не
// собирался вовсе: glob не покрывал os/*.cpp. Точка входа обязана быть
// у приложения, поэтому она здесь.
// ---------------------------------------------------------------------------
#include "os/boot/boot.hpp"
#include "os/boot/boot_app.hpp"
#include "os/common.hpp"
#include "os/config/time_config.hpp"
#include "os/core/memory/memory_manager.hpp"
#include "os/core/task/sheduler.hpp"
#include "os/fault/fault.hpp"
#include "os/power/power.hpp"
#include "os/watchdog/watchdog.hpp"

#include "user_app.hpp"

#ifdef MCFRAME_TARGET_MCU
  #include "power_stm32.hpp"   // энергосбережение платформы (RTC, профиль частоты)
#else
  #include "boot_mock.hpp"             // модель перехода к другой прошивке
  #include "platform_power_mock.hpp"   // модель «железа» энергосбережения на ПК
#endif

using namespace embed;
using namespace embed::config;

// ── Минимальное приложение ──────────────────────────────────────────────────
//
// Приложение, которое лежит В СЛОТЕ (схема с загрузчиком), ограничено размером
// слота — на STM32F103RB это 50 КБ. «Витрина» примера (демонстрация АЦП,
// async-ввода-вывода с UART и I2C, отчёты) занимает десятки килобайт и в слот
// не влезает.
//
// Поэтому демонстрации можно выключить одной настройкой, и тогда остаётся то,
// что действительно нужно работающему устройству: память, время, планировщик,
// задачи, сторож, подсистема OTA и подтверждение собственной загрузки.
// Неиспользуемые функции отчётов при этом не попадают в образ (линковка с
// --gc-sections), поэтому выключение — это не «косметика», а реальные килобайты.
#ifndef MCFRAME_APP_MINIMAL
#define MCFRAME_APP_MINIMAL 0
#endif

namespace {

/// Размеры, которые приложение разрешает использовать ядру.
///
/// Фактические значения обрезаются до ёмкости региона, зарезервированного
/// линкером (см. -DMCU_OS_HEAP_BYTES / -DMCU_OS_STACK_POOL_WORDS).
/// Значение 0 означает «регион не нужен», и регион в линкере тоже 0 —
/// тогда RAM под него не тратится вовсе.
///
/// Примеру нужна небольшая куча для самотеста; приложению со полностью
/// статическими объектами достаточно 0.
#ifdef MCFRAME_TARGET_MCU
// На реальном МКУ регион выделяет линкер (-DMCU_OS_HEAP_BYTES).
constexpr Size kHeapBytes      = 512u;
#else
// На mock регион не ограничен линкером: даём запас для тестов кучи.
constexpr Size kHeapBytes      = 4096u;
#endif
constexpr Size kStackPoolWords = 0u;    // все стеки задач статические

} // namespace

int main() {
    // ── 0. Таблица векторов — на СВОЙ образ ──
    //
    // САМОЕ ПЕРВОЕ действие. Приложение, собранное в слот, после сброса
    // наследует VTOR загрузчика: если не переставить таблицу векторов сейчас,
    // то первое же прерывание (тик, приём байта) уйдёт в чужой код. Здесь это
    // делается явно, потому что SystemInit живёт в библиотеке и собран один
    // раз для всех образов — «смещение на образ» в него не передать.
    if (boot::relocate_vectors_to_self() != Status::Ok) {
        platform_log_line("MCFrame: vector table relocation failed");
    }

    // ── 1. Память ──
    // Адреса регионов приходят от платформы (на STM32 — из линкер-скрипта),
    // поэтому никаких магических 0x20000000 в коде нет.
    if (g_memory_manager.init(kHeapBytes, kStackPoolWords) != Status::Ok) {
        platform_log_line("MCFrame: memory init failed");
        return 1;
    }
    platform_log_line("MCFrame: memory ok");
    if (!g_memory_manager.heap_available()) {
        platform_log_line("MCFrame: heap disabled (linker region is 0)");
    }

    // ── 2. Доверие к платформе ──
    if (!platform::validate()) {
        platform_log_line("MCFrame: platform contract incomplete");
        return 1;
    }

    // ── 3. Энергосбережение платформы (только на МКУ) ──
    //
    // ПОЧЕМУ ЗДЕСЬ, А НЕ ПОЗЖЕ: профиль частоты применяется до настройки
    // периферии и до старта системного тика. SysTick, делители baud у USART,
    // CCR у I2C и прескалер АЦП считаются от PCLK — смена частоты после их
    // настройки ломает обмен по шинам, поэтому переносить этот вызов вниз
    // нельзя.
#ifdef MCFRAME_TARGET_MCU
    {
        stm32::power::Config pcfg{};
        // Профиль выбирается ОДИН раз и на всю загрузку (runtime-переключение
        // частоты выгоднее не делать: см. README). LowPower (8 МГц от HSI)
        // имеет смысл только там, где система обязана работать непрерывно.
        pcfg.profile = platform::PowerProfile::Performance;

        // Глубокий сон (STOP) по умолчанию ВЫКЛЮЧЕН: он требует проверенного
        // источника пробуждения. Включайте, только если платформа собрана с
        // -DMCFRAME_STM32_DEEP_SLEEP=ON и на плате есть кварц 32.768 кГц или
        // вы согласны на неточный LSI.
        pcfg.deep_sleep_enabled = false;
        pcfg.rtc_clock          = stm32::power::RtcClock::Lsi;

        if (stm32::power::init(pcfg) != Status::Ok) {
            platform_log_line("MCFrame: platform power init failed");
            return 1;
        }
    }
#endif

    // ── 4. Время ──
    OsTimer::Config cfg{};
    cfg.min_tick_ms     = 1;
    cfg.max_tick_ms     = 1000;
    cfg.default_tick_ms = 10;
    cfg.alarm_tick_ms   = 1000;
    cfg.max_catchup     = 3;
    if (OsTimer::Init(cfg) != Status::Ok) {
        platform_log_line("MCFrame: timer init failed");
        return 1;
    }

    // ── 5. Супервизор энергосбережения ──
    //
    // Тоже ДО старта тика: смена профиля частоты на ходу запрещена, а init()
    // супервизора — единственное место, где профиль применяется.
    power::Config pcfg{};
    pcfg.window_ms           = 1000u;      // окно измерения нагрузки
    pcfg.allow_deep_sleep    = true;       // решение принимает супервизор
    pcfg.deep_sleep_min_ms   = 8u;         // порог окупаемости входа в STOP
    pcfg.deep_sleep_max_ms   = 60000u;     // просыпаться не реже раза в минуту
    if (power::Supervisor::instance().init(pcfg) != Status::Ok) {
        platform_log_line("MCFrame: power supervisor init failed");
        return 1;
    }

#ifndef MCFRAME_TARGET_MCU
    // На хосте «железо» энергосбережения — модель: разрешаем ей гарантировать
    // сон на 4 секунды, чтобы демонстрация действительно дошла до DeepSleep.
    // На МКУ этот интервал задаёт RTC-будильник, а не тест.
    {
        mock_platform::power_model().deep_horizon_ms     = 4000u;
        mock_platform::power_model().deep_granularity_ms = 1000u;
    }
#endif

    // ── 6. Сторожевой таймер ──
    //
    // init() читает причину прошлой перезагрузки (единственный раз: чтение
    // RCC_CSR снимает флаги) и сообщает супервизору энергосбережения, сколько
    // можно не просыпаться. Железо здесь НЕ запускается: пока приложение
    // инициализируется, сторож не нужен, иначе медленный старт сам приводил бы
    // к сбросу. Запуск — Service::start() в самом конце, когда система готова.
    watchdog::Config wcfg{};
    wcfg.mode              = watchdog::Mode::Warning;
    wcfg.check_period_ms   = 100u;    // задержка обнаружения просрочки
    wcfg.max_stall_ms      = 2000u;   // стирание Flash/калибровка — до 2 с
    wcfg.on_warning        = &user_app_watchdog_warning;
    if (watchdog::Service::instance().init(wcfg) != Status::Ok) {
        platform_log_line("MCFrame: watchdog init failed");
        return 1;
    }

    // ── 7. Планировщик ──
    scheduler& sched = scheduler::instance();
    if (sched.init() != Status::Ok) {
        platform_log_line("MCFrame: scheduler init failed");
        return 1;
    }

    // Если прошлый запуск упал в Fault/panic, запись уже загружена init().
    // Печатаем её до старта задач: это самое полезное, что можно сделать
    // после сброса, и лучше сделать это, пока лог гарантированно работает.
#if MCFRAME_FAULT_REPORT
    if (fault::was_fault_reset()) {
        fault::report();
        fault::clear();
    }
#endif

    // Хуки регистрируем ДО старта тика. Порядок важен: супервизор должен
    // увидеть, что исполнялось в момент прерывания, ДО того как планировщик
    // в этом же тике сменит задачу.
    if (OsTimer::RegisterSystemHook(&power::Supervisor::tick_hook, nullptr) != Status::Ok) {
        platform_log_line("MCFrame: power hook registration failed");
        return 1;
    }
    // Сторож проверяет сроки участников в том же тике: если тик умрёт, сторож
    // перестанет кормить таймер и устройство перезагрузится.
    if (OsTimer::RegisterSystemHook(&watchdog::Service::tick_hook, nullptr) != Status::Ok) {
        platform_log_line("MCFrame: watchdog hook registration failed");
        return 1;
    }
    if (OsTimer::RegisterSystemHook(&scheduler::tick_hook, nullptr) != Status::Ok) {
        platform_log_line("MCFrame: hook registration failed");
        return 1;
    }

    if (OsTimer::Start() != Status::Ok) {
        platform_log_line("MCFrame: timer start failed");
        return 1;
    }

    // ── 8. Задачи приложения ──
    if (user_app_register(sched) != Status::Ok) {
        platform_log_line("MCFrame: task registration failed");
        return 1;
    }

#if !MCFRAME_APP_MINIMAL
    if (user_app_selftest() != Status::Ok) {
        platform_log_line("MCFrame: selftest failed");
        return 1;
    }

    if (user_app_adc_demo() != Status::Ok) {
        platform_log_line("MCFrame: ADC demo failed");
        return 1;
    }

    if (user_app_async_demo() != Status::Ok) {
        platform_log_line("MCFrame: async io demo failed");
        return 1;
    }
#endif

#ifndef MCFRAME_TARGET_MCU
    // На стенде «прошивка уже загружена загрузчиком в слот A»: адрес таблицы
    // векторов ставим тот, который поставил бы загрузчик. Иначе пример честно
    // сообщил бы «обновление недоступно» (образ вне слотов) — и вся ветка OTA
    // на хосте осталась бы непроверенной.
    mock_platform::boot_model().vectors = boot::kBoard.slot_offset[0];
#endif

    // ── OTA: где лежит новая прошивка и по чему принимается ──
    //
    // Инициализация идёт ПОСЛЕ запуска времени: сервису нужны таймауты сессии
    // и сторож (окно на длительное стирание). Задача приёма уже зарегистрирована
    // в user_app_register, но начнёт исполняться только после sched.start() —
    // до этого интерфейс никто не опрашивает, и это правильно: половина
    // подсистем ещё не готова.
    if (user_app_ota_init() != Status::Ok) {
        platform_log_line("MCFrame: OTA init failed");
        return 1;
    }

    // Участники сторожа: то, что обязано работать. Регистрируются ДО start()
    // сторожа — аппаратный таймаут считается по их срокам.
    if (user_app_watchdog_register() != Status::Ok) {
        platform_log_line("MCFrame: watchdog participants failed");
        return 1;
    }

    platform_log_line("MCFrame: tasks registered, starting");
#if !MCFRAME_APP_MINIMAL
    user_app_report_ram();
#endif

    // ── 9. Запуск сторожа и планировщика ──
    //
    // Сторож включается последним: с этого момента он необратим (IWDG нельзя
    // выключить), и всё, что работало до этого, должно быть уже готово.
    if (watchdog::Service::instance().start() != Status::Ok) {
        platform_log_line("MCFrame: watchdog start failed");
        return 1;
    }
    // На реальном МКУ не возвращается. На mock/хосте переключение контекста
    // невозможно, поэтому start() возвращается, и мы крутим тик вручную —
    // это позволяет тестировать логику планировщика без железа.
    const Status st = sched.start();
    if (st != Status::Ok) {
        platform_log_line("MCFrame: start failed");
        return 1;
    }

#ifndef MCFRAME_TARGET_MCU
    // Mock-режим: прогоняем тики, чтобы проверить обработку сна и
    // перепланирование, затем выходим — на хосте реального переключения
    // контекста нет, поэтому «вечный» цикл здесь бессмысленен.
    //
    // ВАЖНО про энергосбережение: на хосте idle-задача не ИСПОЛНЯЕТСЯ (нет
    // переключения контекста), поэтому простой отдаём супервизору вручную —
    // ровно то, что на МКУ делает idle-задача. Так решение «спать глубоко или
    // нет» проверяется без железа.
    //
    // Загрузка CPU здесь получится «100 %»: на хосте задача не исполняется,
    // но её состояние — running, а супервизор именно это и измеряет. Честные
    // цифры нагрузки даёт только плата.
    for (uint32_t i = 0; i < 120u; ++i) {
        OsTimer::TickIsr();
        if (i == 20u) power::Supervisor::instance().on_idle();
    }
#if !MCFRAME_APP_MINIMAL
    user_app_report_power();
    user_app_report_watchdog();
    user_app_report_ota();
#endif
    platform_log_line("MCFrame: simulated run finished");
    return 0;
#else
    // На реальном МКУ управление сюда не доходит: start() передаёт его
    // задачам через PendSV/SysTick. this loop — защита от возврата.
    for (;;) {
        scheduler::instance().yield();
    }
#endif
}
