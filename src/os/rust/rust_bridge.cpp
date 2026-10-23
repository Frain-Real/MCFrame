// os/rust/rust_bridge.cpp
#include "rust_bridge.hpp"

#include "../config/time_config.hpp"
#include "../core/task/sheduler.hpp"
#include "../iplatform.hpp"

extern "C" void mcframe_rust_main(void) __attribute__((weak));

extern "C" {

/// Записать строку из Rust. Строка обязана быть NUL-terminated.
void mc_log(const char* text) noexcept {
    embed::platform_log_line((text == nullptr) ? "" : text);
}

/// Спать из задачи Rust. Вне запущенного планировщика — Unavailable.
int mc_sleep_ms(uint32_t ms) noexcept {
    if (!embed::scheduler::instance().started()) return static_cast<int>(embed::Status::Unavailable);
    return static_cast<int>(embed::scheduler::instance().sleep_for_ms(ms));
}

int mc_yield(void) noexcept {
    if (!embed::scheduler::instance().started()) return static_cast<int>(embed::Status::Unavailable);
    embed::scheduler::instance().yield();
    return static_cast<int>(embed::Status::Ok);
}

uint64_t mc_uptime_ms(void) noexcept {
    return embed::config::OsTimer::NowMs();
}

uint64_t mc_epoch_seconds(void) noexcept {
    return embed::config::OsTimer::NowEpochSeconds();
}

} // extern "C"

namespace embed::rust {

bool available() noexcept {
    return mcframe_rust_main != nullptr;
}

bool run() noexcept {
    if (mcframe_rust_main == nullptr) return false;
    mcframe_rust_main();
    return true;
}

} // namespace embed::rust
