// boards/f103_generic/board.hpp
// ---------------------------------------------------------------------------
// Generic-плата на STM32F103: USART1/2/3, SPI1/2, I2C1, ADC1, PWM TIM2_CH1,
// LED PC13. Карта пинов — boards/f103_generic/pins.hpp.
// ---------------------------------------------------------------------------
#pragma once

#include "boards/f103_generic/pins.hpp"

#include "hal/IGpio.hpp"
#include "hal/ISpi.hpp"
#include "hal/IPwm.hpp"
#include "hal/IUart.hpp"
#include "hal/INvs.hpp"

#include "uart_stm32.hpp"
#include "i2c_stm32.hpp"
#include "stm32_periph.hpp"

namespace board {

using Uart  = embed::stm32::UartDriver1;
using Uart2 = embed::stm32::UartDriver2;
using Uart3 = embed::stm32::UartDriver3;
using I2c   = embed::stm32::I2cDriver1;

using Console = embed::hal::IUart;
using Spi     = embed::hal::ISpi;
using Pwm     = embed::hal::IPwm;
using Gpio    = embed::hal::IGpioPin;
using Nvs     = embed::hal::INvs;

Uart&  uart1() noexcept;
Uart2& uart2() noexcept;
Uart3& uart3() noexcept;
Nvs&   nvs() noexcept;
Console& console() noexcept;
Gpio& led() noexcept;
Gpio& spi_cs() noexcept;
Gpio& spi2_cs() noexcept;
Spi& spi1() noexcept;
Spi& spi2() noexcept;
Pwm& pwm1() noexcept;

template <uint8_t I>
inline auto& spi() noexcept {
    if constexpr (I == 1u) return spi1();
    else if constexpr (I == 2u) return spi2();
    else static_assert(I == 1u || I == 2u, "f103_generic: нет такого SPI");
}

using Pins = PinMap<20>;

constexpr const Pins& pins() noexcept { return kPins; }

template <uint8_t I>
inline auto& uart() noexcept {
    if constexpr (I == 1u) return uart1();
    else if constexpr (I == 2u) return uart2();
    else if constexpr (I == 3u) return uart3();
    else static_assert(I >= 1u && I <= 3u, "f103_generic: нет такого UART");
}

void init() noexcept;

} // namespace board
