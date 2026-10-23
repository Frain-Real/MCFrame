// boards/esp32c3/board.hpp
// ---------------------------------------------------------------------------
// ESP32-C3 DevKit-подобная плата: UART0, SPI2, LEDC PWM, LED/CS GPIO.
// Пины можно переопределить -D до сборки IDF-компонента.
// ---------------------------------------------------------------------------
#pragma once

#include "hal/IGpio.hpp"
#include "hal/ISpi.hpp"
#include "hal/IPwm.hpp"
#include "hal/IUart.hpp"
#include "hal/INvs.hpp"
#include "hal/INetwork.hpp"
#include "os/net/socket.hpp"
#include "hal/IBle.hpp"

#ifndef BOARD_LED_GPIO
#define BOARD_LED_GPIO 8
#endif
#ifndef BOARD_SPI_MOSI_GPIO
#define BOARD_SPI_MOSI_GPIO 7
#endif
#ifndef BOARD_SPI_MISO_GPIO
#define BOARD_SPI_MISO_GPIO 2
#endif
#ifndef BOARD_SPI_SCLK_GPIO
#define BOARD_SPI_SCLK_GPIO 6
#endif
#ifndef BOARD_SPI_CS_GPIO
#define BOARD_SPI_CS_GPIO 10
#endif
#ifndef BOARD_PWM_GPIO
#define BOARD_PWM_GPIO 3
#endif

namespace board {

using Uart    = embed::hal::IUart;
using Console = embed::hal::IUart;
using Spi     = embed::hal::ISpi;
using Pwm     = embed::hal::IPwm;
using Gpio    = embed::hal::IGpioPin;
using Nvs     = embed::hal::INvs;
using Network = embed::hal::INetwork;
using Sockets = embed::net::Factory;
using Ble     = embed::hal::IBle;

Uart& uart1() noexcept;
Console& console() noexcept;
Gpio& led() noexcept;
Gpio& spi_cs() noexcept;
Spi& spi1() noexcept;
Pwm& pwm1() noexcept;
Nvs& nvs() noexcept;
Network& wifi() noexcept;
Sockets& sockets() noexcept;
Ble& ble() noexcept;

void init() noexcept;

} // namespace board
