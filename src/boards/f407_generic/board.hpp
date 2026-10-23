// boards/f407_generic/board.hpp
// ---------------------------------------------------------------------------
// Generic-плата STM32F407: USART1/2, I2C1/2, SPI1/2, ADC1, PWM TIM2_CH1,
// LED PC13. Все пины берутся из boards/f407_generic/pins.hpp.
// ---------------------------------------------------------------------------
#pragma once

#include "boards/f407_generic/pins.hpp"

#include "hal/IGpio.hpp"
#include "hal/ISpi.hpp"
#include "hal/IPwm.hpp"
#include "hal/IUart.hpp"
#include "hal/II2c.hpp"
#include "hal/IAdc.hpp"
#include "hal/INvs.hpp"
#include "os/net/w5500.hpp"

#include "stm32_periph_m4.hpp"

namespace board {

using Uart    = embed::hal::IUart;
using I2c     = embed::hal::II2c;
using Adc     = embed::hal::IAdc;
using Console = embed::hal::IUart;
using Spi     = embed::hal::ISpi;
using Pwm     = embed::hal::IPwm;
using Gpio    = embed::hal::IGpioPin;
using Nvs     = embed::hal::INvs;
using Net     = embed::net::W5500FactoryT<4>;

using Pins = PinMap<20>;
constexpr const Pins& pins() noexcept { return kPins; }

Uart& uart1() noexcept;
Uart& uart2() noexcept;
I2c&  i2c1() noexcept;
I2c&  i2c2() noexcept;
Adc&  adc1() noexcept;
Console& console() noexcept;
Gpio& led() noexcept;
Gpio& spi_cs() noexcept;
Spi&  spi1() noexcept;
Spi&  spi2() noexcept;
Pwm&  pwm1() noexcept;
Nvs&  nvs() noexcept;
Net&  net() noexcept;

/// Инстанс-шаблоны: board::uart<2>(), board::spi<2>(), board::i2c<1>().
/// Так приложение не зависит от конкретного имени функции и может
/// параметризовать драйвер номером периферии.
template <uint8_t I>
inline auto& uart() noexcept {
    if constexpr (I == 1u) return uart1();
    else if constexpr (I == 2u) return uart2();
    else static_assert(I == 1u || I == 2u, "f407_generic: нет такого UART");
}

template <uint8_t I>
inline auto& spi() noexcept {
    if constexpr (I == 1u) return spi1();
    else if constexpr (I == 2u) return spi2();
    else static_assert(I == 1u || I == 2u, "f407_generic: нет такого SPI");
}

template <uint8_t I>
inline auto& i2c() noexcept {
    if constexpr (I == 1u) return i2c1();
    else if constexpr (I == 2u) return i2c2();
    else static_assert(I == 1u || I == 2u, "f407_generic: нет такого I2C");
}

void init() noexcept;
/// Инициализация только периферии (LED/CS/UART/ADC), без NVS.
/// Нужна self-test-образам, которые не должны тянуть NVS в свой слот.
void init_peripherals() noexcept;

} // namespace board
