// tests/test_time_units.cpp
// ---------------------------------------------------------------------------
// Duration/TimePoint: перевод единиц и округление вверх.
//
// Главная проверка — не «формула работает», а «сон не короче запрошенного»:
// 1 мс при тике 10 мс обязана превратиться в 1 тик (10 мс), а не в 0.
// ---------------------------------------------------------------------------
#include "os/time/duration.hpp"
#include "os/core/task/sheduler.hpp"

#include <cstdio>

namespace {

int g_failed = 0;
int g_total = 0;

void check(const char* what, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

using embed::chrono::Duration;
using embed::chrono::TimePoint;

static_assert(Duration::from_ms(10).ticks_ceil(10) == 1u, "10 мс при тике 10 мс — 1 тик");
static_assert(Duration::from_ms(11).ticks_ceil(10) == 2u, "11 мс округляется вверх до 2 тиков");
static_assert(Duration::from_ms(1).ticks_ceil(10) == 1u, "1 мс не должна стать нулём тиков");
static_assert(Duration::from_ms(0).ticks_ceil(10) == 0u, "0 мс — это yield, а не тик");
static_assert(Duration::from_ticks(3, 10).ms() == 30u, "3 тика по 10 мс — 30 мс");
static_assert(Duration::from_ms(1500).ms_ceil() == 1500u, "ms_ceil миллисекунд");
static_assert(Duration::from_us(1500).ms_ceil() == 2u, "1.5 мс округляется вверх до 2");
static_assert(TimePoint::from_ms(100).ms() == 100u, "TimePoint ms");

} // namespace

int main() {
    std::printf("=== Duration / TimePoint ===\n");

    check("секунды в миллисекунды", embed::chrono::seconds(2).ms() == 2000u);
    check("миллисекунды в микросекунды", embed::chrono::milliseconds(2).us() == 2000u);
    check("сложение", (embed::chrono::milliseconds(200) + embed::chrono::milliseconds(300)).ms() == 500u);
    check("вычитание без отрицательных", (embed::chrono::milliseconds(100) - embed::chrono::milliseconds(300)).is_zero());
    check("умножение", (embed::chrono::milliseconds(10) * 5u).ms() == 50u);
    check("сравнение", embed::chrono::milliseconds(100) < embed::chrono::milliseconds(200));

    const TimePoint t0 = TimePoint::from_ms(1000u);
    const TimePoint t1 = t0 + embed::chrono::milliseconds(250u);
    check("TimePoint + Duration", t1.ms() == 1250u);
    check("TimePoint - TimePoint", (t1 - t0).ms() == 250u);
    check("отрицательная разница — ноль", (t0 - t1).is_zero());
    check("порядок TimePoint", t0 < t1);

    check("тик по умолчанию не ноль", embed::scheduler::tick_ms() != 0u);

    std::printf("%d/%d проверок пройдено%s\n", g_total - g_failed, g_total,
                g_failed == 0 ? " — все тесты пройдены" : " — ЕСТЬ ОШИБКИ");
    return g_failed == 0 ? 0 : 1;
}
