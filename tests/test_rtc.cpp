// tests/test_rtc.cpp
#include "os/time/rtc.hpp"
#include "os/config/time_config.hpp"

#include <cstdio>

using namespace embed;

namespace {
int g_total = 0;
int g_failed = 0;
void check(const char* what, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}
}

int main() {
    std::printf("=== RTC wall clock ===\n");

    config::OsTimer::Config tcfg{};
    tcfg.min_tick_ms = 1;
    tcfg.max_tick_ms = 1000;
    tcfg.default_tick_ms = 1;
    check("timer init", config::OsTimer::Init(tcfg) == Status::Ok);

    rtc::SoftBackend backend;
    rtc::WallClock& clock = rtc::WallClock::instance();
    clock.attach(&backend);

    check("empty RTC is TimeNotSynced",
          clock.sync_from_rtc() == Status::TimeNotSynced);
    check("no OS time yet", !config::OsTimer::TimeValid());

    // 2026-01-01T00:00:00Z
    constexpr uint64_t kEpoch = 1767225600ull;
    check("set_epoch", clock.set_epoch(kEpoch) == Status::Ok);
    check("OS time is valid", config::OsTimer::TimeValid());
    check("OS epoch", config::OsTimer::NowEpochSeconds() == kEpoch);
    check("RTC got epoch", backend.valid());

    // Сброс ОС: монотонное время и wall-offset обнуляются.
    check("timer re-init (reset)", config::OsTimer::Init(tcfg) == Status::Ok);
    check("OS time reset", !config::OsTimer::TimeValid());
    check("sync from RTC", clock.sync_from_rtc() == Status::Ok);
    check("OS epoch restored", config::OsTimer::NowEpochSeconds() == kEpoch);

    const DateTime dt = clock.now();
    check("calendar year", dt.year == 2026u && dt.month == 1u && dt.day == 1u);

    // Потеря RTC (например, села батарейка) не должна выдавать ложное время.
    backend.invalidate();
    check("invalid RTC rejected", clock.sync_from_rtc() == Status::TimeNotSynced);

    // persist() должен сохранить уже установленное системное время.
    clock.set_epoch(kEpoch + 10u);
    backend.invalidate();
    check("persist after re-set", clock.persist() == Status::Ok);
    check("persisted value", backend.valid());

    std::printf("%d/%d проверок пройдено%s\n", g_total - g_failed, g_total,
                g_failed == 0 ? " — все тесты пройдены" : " — ЕСТЬ ОШИБКИ");
    return g_failed == 0 ? 0 : 1;
}
