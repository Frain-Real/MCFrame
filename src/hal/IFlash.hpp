// hal/IFlash.hpp
#pragma once

#include "os/common.hpp"

namespace embed::hal {

class IFlash {
public:
    virtual ~IFlash() = default;

    virtual uint32_t page_size() const noexcept = 0;
    virtual bool     is_volatile() const noexcept = 0;

    virtual Status read(uint32_t address, void* buffer, Size len) noexcept = 0;
    virtual Status write(uint32_t address, const void* data, Size len) noexcept = 0;
    virtual Status erase(uint32_t address, Size len) noexcept = 0;
};

} // namespace embed::hal
