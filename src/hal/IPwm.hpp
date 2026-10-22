// hal/IPwm.hpp
#pragma once

#include "os/common.hpp"

namespace embed::hal {

enum class PwmPolarity : uint8_t { ActiveHigh, ActiveLow };

struct PwmConfig {
    uint32_t     frequency_hz = 1'000u;
    uint8_t      duty_percent = 0u;
    PwmPolarity  polarity     = PwmPolarity::ActiveHigh;
};

class IPwm {
public:
    virtual ~IPwm() = default;

    virtual Status configure(const PwmConfig& cfg) noexcept = 0;
    virtual Status set_duty(uint8_t duty_percent) noexcept = 0;
    virtual Status start() noexcept = 0;
    virtual Status stop() noexcept = 0;
    virtual uint32_t caps() const noexcept = 0;
};

} // namespace embed::hal
