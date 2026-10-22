// hal/II2c.hpp
#pragma once

#include "os/common.hpp"

namespace embed::hal {

struct I2cConfig {
    uint32_t hz = 100'000u;
};

class II2c {
public:
    virtual ~II2c() = default;

    virtual Status configure(const I2cConfig& cfg) noexcept = 0;
    virtual Status write(uint8_t address, const void* data, Size len,
                         Timeout timeout) noexcept = 0;
    virtual Status read(uint8_t address, void* buffer, Size capacity,
                        Size& bytes_read, Timeout timeout) noexcept = 0;
    virtual Status lock(Timeout timeout) noexcept = 0;
    virtual void   unlock() noexcept = 0;
};

} // namespace embed::hal
