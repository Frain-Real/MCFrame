// example/app_main.cpp
// ---------------------------------------------------------------------------
// Точка входа приложения. Порядок инициализации ядра ОС.
//
// Прежний main.cpp лежал в библиотеке (src/os/main.cpp) и при этом не
// собирался вовсе: glob не покрывал os/*.cpp. Точка входа обязана быть
// у приложения, поэтому она здесь.
// ---------------------------------------------------------------------------
#include "os/common.hpp"
#include "os/config/time_config.hpp"
#include "os/core/memory/memory_manager.hpp"
#include "os/core/task/sheduler.hpp"

#include "user_app.hpp"

using namespace embed;
using namespace embed::config;

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

    // ── 3. Время ──
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

    // ── 4. Планировщик ──
    scheduler& sched = scheduler::instance();
    if (sched.init() != Status::Ok) {
        platform_log_line("MCFrame: scheduler init failed");
        return 1;
    }

    // Статический хук: контекст не нужен, поэтому невозможно передать
    // nullptr и упасть при первом тике (как было в прежнем main.cpp).
    if (OsTimer::RegisterSystemHook(&scheduler::tick_hook, nullptr) != Status::Ok) {
        platform_log_line("MCFrame: hook registration failed");
        return 1;
    }

    // Тик запускаем ПОСЛЕ регистрации хука.
    if (OsTimer::Start() != Status::Ok) {
        platform_log_line("MCFrame: timer start failed");
        return 1;
    }

    // ── 5. Задачи приложения ──
    if (user_app_register(sched) != Status::Ok) {
        platform_log_line("MCFrame: task registration failed");
        return 1;
    }

    if (user_app_selftest() != Status::Ok) {
        platform_log_line("MCFrame: selftest failed");
        return 1;
    }

    if (user_app_adc_demo() != Status::Ok) {
        platform_log_line("MCFrame: ADC demo failed");
        return 1;
    }

    platform_log_line("MCFrame: tasks registered, starting");
    user_app_report_ram();

    // ── 6. Старт ──
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
    for (uint32_t i = 0; i < 40u; ++i) {
        OsTimer::TickIsr();
    }
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
