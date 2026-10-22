// hal/IGpio.hpp
// ---------------------------------------------------------------------------
// GPIO-интерфейс: приложение не знает, F1 это, F4 или ESP32-C3.
// ---------------------------------------------------------------------------
#pragma once

#include "os/common.hpp"

namespace embed::hal {

enum class GpioMode : uint8_t { Input, Output, Alternate, Analog };
enum class GpioPull : uint8_t { None, Up, Down };
enum class GpioSpeed : uint8_t { Low, Medium, High, VeryHigh };
enum class GpioEdge : uint8_t { Rising, Falling, Any };

struct GpioConfig {
    GpioMode  mode  = GpioMode::Input;
    GpioPull  pull  = GpioPull::None;
    GpioSpeed speed = GpioSpeed::Low;
    bool      initial = false;   // для Output
};

using GpioIrqCallback = void (*)(void*);

class IGpioPin {
public:
    virtual ~IGpioPin() = default;

    virtual Status configure(const GpioConfig& cfg) noexcept = 0;
    virtual void   set(bool level) noexcept = 0;
    virtual bool   get() const noexcept = 0;
    virtual void   toggle() noexcept = 0;

    /// Прерывание по фронту. Реализация сама решает, EXTI это или GPIO-матрица.
    virtual Status attach_irq(GpioEdge edge, GpioIrqCallback cb, void* ctx) noexcept = 0;
    virtual void   detach_irq() noexcept = 0;
};

} // namespace embed::hal
