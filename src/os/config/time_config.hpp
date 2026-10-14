#pragma once

#include "../time/timer.hpp"
#include "os_config.hpp"

namespace embed::config {

    // ────────────────────────────────────────────────────────────────────
    // Ёмкости модуля времени.
    //
    // ЗАЧЕМ МЕНЬШЕ: прежние 16 таймеров + 8 будильников занимали 804 Б
    // статики (TimerSlot ≈ 32 Б каждый), даже если приложение не создавало
    // ни одного таймера. Выбирайте по фактическому числу таймеров/будильников.
    //
    // Переопределение: -DMCFRAME_TIMER_COUNT=8 -DMCFRAME_ALARM_COUNT=4
    // ────────────────────────────────────────────────────────────────────
    #ifndef MCFRAME_TIMER_COUNT
    #define MCFRAME_TIMER_COUNT 4u
    #endif
    #ifndef MCFRAME_ALARM_COUNT
    #define MCFRAME_ALARM_COUNT 2u
    #endif
    #ifndef MCFRAME_HOOK_COUNT
    // Два хука занимает само ядро: scheduler::tick_hook и
    // power::Supervisor::tick_hook (учёт нагрузки и решение о сне).
    // Третий — запас для приложения; HookSlot стоит 12 Б статики.
    #define MCFRAME_HOOK_COUNT 3u
    #endif

    using OsTimer = TimerModule<
        MCFRAME_TIMER_COUNT,   // MaxTimers
        MCFRAME_ALARM_COUNT,   // MaxAlarms
        MCFRAME_HOOK_COUNT     // MaxSystemHooks
    >;

}