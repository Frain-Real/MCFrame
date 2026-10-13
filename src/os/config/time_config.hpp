#pragma once

#include "../time/timer.hpp"
namespace embed::config {

    using OsTimer = TimerModule<
        16,     // MaxTimers
        8,      // MaxAlarms
        2       // MaxSystemHooks
    >;


}