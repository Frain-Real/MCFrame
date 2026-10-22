// hal/ISpi.hpp
// ---------------------------------------------------------------------------
// SPI-шина как интерфейс. Устройство зависит от ISpi/SpiDevice, а не от SoC.
// ---------------------------------------------------------------------------
#pragma once

#include "os/common.hpp"

namespace embed::hal {

struct SpiConfig {
    uint32_t hz        = 1'000'000u;
    uint8_t  mode      = 0u;      // 0..3
    bool     msb_first = true;
    bool     dma       = false;
    bool     irq       = false;
};

enum SpiCaps : uint32_t {
    SpiCapNone = 0u,
    SpiCapIrq  = 1u << 0,
    SpiCapDma  = 1u << 1,
    SpiCapCs   = 1u << 2,
};

class ISpi {
public:
    virtual ~ISpi() = default;

    virtual Status configure(const SpiConfig& cfg) noexcept = 0;

    /// Полный дуплекс: tx/rx могут быть nullptr (только приём/только передача).
    virtual Status transfer(const void* tx, void* rx, Size n, Timeout timeout) noexcept = 0;

    /// Блокировка шины для нескольких устройств на одной линии.
    virtual Status lock(Timeout timeout) noexcept = 0;
    virtual void   unlock() noexcept = 0;

    virtual uint32_t caps() const noexcept = 0;
};

} // namespace embed::hal
