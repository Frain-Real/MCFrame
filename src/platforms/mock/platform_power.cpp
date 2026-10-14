// platforms/mock/platform_power.cpp
// ---------------------------------------------------------------------------
// Реализация platform::PowerOps для mock-сборки: модель энергосбережения
// (см. platform_power_mock.hpp) вместо железа.
//
// Никакой «магии» здесь нет намеренно: функции обязаны вести себя так же
// честно, как железо, — отказать, если источник пробуждения не вооружён,
// и вернуть ИЗМЕРЕННОЕ время сна, а не запрошенное.
// ---------------------------------------------------------------------------
#include "platform_power_mock.hpp"

namespace embed::mock_platform {

PowerModel& power_model() noexcept {
    static PowerModel m;
    return m;
}

} // namespace embed::mock_platform

namespace embed::platform {

namespace {

using embed::mock_platform::power_model;
using PowerModel = embed::mock_platform::PowerModel;

uint32_t mock_power_caps() OS_PLATFORM_FN {
    return power_model().caps;
}

uint32_t mock_power_sysclk_hz() OS_PLATFORM_FN {
    return power_model().sysclk_hz;
}

uint32_t mock_power_set_profile(uint32_t profile) OS_PLATFORM_FN {
    PowerModel& m = power_model();
    ++m.profile_calls;
    m.last_profile = profile;

    switch (static_cast<PowerProfile>(profile)) {
        case PowerProfile::Performance: m.sysclk_hz = m.performance_hz; return m.sysclk_hz;
        case PowerProfile::LowPower:    m.sysclk_hz = m.low_power_hz;   return m.sysclk_hz;
    }
    // Неизвестный профиль — отказ, а не «сделаем вид, что применили».
    return 0u;
}

uint32_t mock_power_deep_horizon_ms() OS_PLATFORM_FN {
    // Горизонт — это «сколько платформа обещает». Отказ во входе
    // (deep_enter == false) его не обнуляет: так выглядит железо, у которого
    // будильник настроен, но конкретный вход не состоялся.
    return power_model().deep_horizon_ms;
}

uint32_t mock_power_deep_granularity_ms() OS_PLATFORM_FN {
    return power_model().deep_granularity_ms;
}

uint32_t mock_power_deep_sleep(uint32_t max_ms) OS_PLATFORM_FN {
    PowerModel& m = power_model();
    ++m.deep_calls;
    m.last_request_ms = max_ms;
    m.last_slept_ms   = 0u;

    if (!m.deep_enter || m.deep_horizon_ms == 0u) return 0u;

    // «Проспали» либо заданное тестом время, либо запрошенный горизонт.
    const uint32_t slept = (m.deep_slept_ms != 0u) ? m.deep_slept_ms : max_ms;
    m.last_slept_ms = slept;
    return slept;
}

const PowerOps kMockPowerOps{
    /* caps                     */ &mock_power_caps,
    /* sysclk_hz                */ &mock_power_sysclk_hz,
    /* set_profile              */ &mock_power_set_profile,
    /* deep_sleep_horizon_ms    */ &mock_power_deep_horizon_ms,
    /* deep_sleep_granularity_ms*/ &mock_power_deep_granularity_ms,
    /* deep_sleep               */ &mock_power_deep_sleep,
};

} // namespace

const PowerOps& mock_power_ops() noexcept { return kMockPowerOps; }

} // namespace embed::platform
