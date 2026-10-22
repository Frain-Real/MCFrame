// hal/SpiDevice.hpp
// ---------------------------------------------------------------------------
// Устройство на SPI-шине: CS + блокировка шины. Драйверы Flash/SD/радио
// зависят от SpiDevice, а не от ISpi напрямую — тогда несколько устройств
// могут делить одну шину.
// ---------------------------------------------------------------------------
#pragma once

#include "ISpi.hpp"
#include "IGpio.hpp"

namespace embed::hal {

class SpiDevice {
public:
    SpiDevice(ISpi& bus, IGpioPin& cs, bool cs_active_low = true) noexcept
        : bus_(bus), cs_(cs), cs_active_low_(cs_active_low) {}

    SpiDevice(const SpiDevice&)            = delete;
    SpiDevice& operator=(const SpiDevice&) = delete;

    Status transfer(const void* tx, void* rx, Size n,
                    Timeout timeout = 1000u) noexcept {
        const Status locked = bus_.lock(timeout);
        if (locked != Status::Ok) return locked;

        select(true);
        const Status st = bus_.transfer(tx, rx, n, timeout);
        select(false);
        bus_.unlock();
        return st;
    }

    Status write(const void* tx, Size n, Timeout timeout = 1000u) noexcept {
        return transfer(tx, nullptr, n, timeout);
    }

    Status read(void* rx, Size n, Timeout timeout = 1000u) noexcept {
        return transfer(nullptr, rx, n, timeout);
    }

    ISpi& bus() noexcept { return bus_; }
    IGpioPin& cs() noexcept { return cs_; }

private:
    void select(bool active) noexcept {
        const bool level = cs_active_low_ ? !active : active;
        cs_.set(level);
    }

    ISpi&     bus_;
    IGpioPin& cs_;
    bool      cs_active_low_;
};

} // namespace embed::hal
