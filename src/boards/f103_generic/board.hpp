// boards/f103_generic/board.hpp
// ---------------------------------------------------------------------------
// Generic-плата на STM32F103: USART1, I2C1, LED PC13.
// SPI/PWM в Phase 3 появятся как реальные драйверы; пока board::spi1()/
// pwm1() возвращают NotSupported-реализации, чтобы приложение уже зависело
// от интерфейсов, а не от STM32.
// ---------------------------------------------------------------------------
#pragma once

#include "hal/IGpio.hpp"
#include "hal/ISpi.hpp"
#include "hal/IPwm.hpp"
#include "hal/IUart.hpp"

#include "uart_stm32.hpp"
#include "i2c_stm32.hpp"
#include "stm32_periph.hpp"

namespace board {

using Uart = embed::stm32::UartDriver1;
using I2c  = embed::stm32::I2cDriver1;

using Console = embed::hal::IUart;
using Spi     = embed::hal::ISpi;
using Pwm     = embed::hal::IPwm;
using Gpio    = embed::hal::IGpioPin;

Uart& uart1() noexcept;
Console& console() noexcept;
Gpio& led() noexcept;
Gpio& spi_cs() noexcept;
Spi& spi1() noexcept;
Pwm& pwm1() noexcept;

void init() noexcept;

} // namespace board
