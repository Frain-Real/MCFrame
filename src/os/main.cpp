#include "common.hpp"
#include "config/dma_config.hpp"
#include "config/time_config.hpp"
#include "core/task/sheduler.hpp"

using namespace embed;
using namespace embed::config;

int main(){
    OsTimer::Config cfg;
    scheduler sched;
    cfg.min_tick_ms = 1;
    cfg.max_tick_ms = 1000;
    cfg.default_tick_ms = 10;
    cfg.alarm_tick_ms = 1000;
    cfg.max_catchup = 3;
    cfg.start_time_valid = false;

    OsTimer::Init(cfg);
    OsTimer::RegisterSystemHook([](void* ctx) {
        static_cast<scheduler*>(ctx)->tick();
    }, nullptr);
    
    sched.init();
    while (true) {
        // Основной цикл или переход в сон
    }
    return 0;
}