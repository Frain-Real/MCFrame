// Generic-плата на STM32F407/G431: SPI1 (PA5/6/7, CS PA4), PWM TIM2_CH1 (PA0),
// LED PC13. USART пока не реализован — console() возвращает NotSupported.
#pragma once

#include "hal/IGpio.hpp"
#include "hal/ISpi.hpp"
#include "hal/IPwm.hpp"
#include "hal/IUart.hpp"
#include "stm32_periph_m4.hpp"

namespace board {

using Uart = embed::hal::IUart;
using Console = embed::hal::IUart;
using Spi = embed::hal::ISpi;
using Pwm = embed::hal::IPwm;
using Gpio = embed::hal::IGpioPin;

Uart& uart1() noexcept;
Console& console() noexcept;
Gpio& led() noexcept;
Gpio& spi_cs() noexcept;
Spi& spi1() noexcept;
Pwm& pwm1() noexcept;

void init() noexcept;

} // namespace board
