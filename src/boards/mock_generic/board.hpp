// boards/mock_generic/board.hpp
// ---------------------------------------------------------------------------
// Mock-плата: те же board::-имена, что у реальной, но HAL-реализации
// программные. Нужна для host-тестов и примера на ПК.
// ---------------------------------------------------------------------------
#pragma once

#include "hal/IGpio.hpp"
#include "hal/ISpi.hpp"
#include "hal/IPwm.hpp"
#include "hal/IUart.hpp"
#include "hal/mock/mock_hal.hpp"

#include "uart_mock.hpp"
#include "i2c_mock.hpp"

namespace board {

using Uart = embed::mock::UartMockDma;
using I2c  = embed::mock::I2cMockDma;

using Console = embed::hal::IUart;
using Spi     = embed::hal::ISpi;
using Pwm     = embed::hal::IPwm;
using Gpio    = embed::hal::IGpioPin;

Uart& uart1() noexcept;
Console& console() noexcept;
Gpio& led() noexcept;
Spi& spi1() noexcept;
Pwm& pwm1() noexcept;

void init() noexcept;

} // namespace board
