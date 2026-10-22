// hal/IAdc.hpp
#pragma once

#include "os/common.hpp"

namespace embed::hal {

struct AdcConfig {
    uint8_t resolution_bits = 12u;
    uint32_t sample_rate_hz = 0u;   // 0 — максимальная/по умолчанию
};

class IAdc {
public:
    virtual ~IAdc() = default;

    virtual Status configure(const AdcConfig& cfg) noexcept = 0;
    virtual Status read(uint8_t channel, uint16_t& value) noexcept = 0;
};

} // namespace embed::hal
