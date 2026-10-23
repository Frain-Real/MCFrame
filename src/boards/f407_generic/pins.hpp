// boards/f407_generic/pins.hpp
// ---------------------------------------------------------------------------
// Карта пинов generic-платы STM32F407.
//
// AF-номера — из RM0090:
//   USART1/2/3 — AF7, SPI1/2 — AF5, I2C1/2 — AF4, TIM2 — AF1.
//
// I2C2 занимает PB10/PB11, которые на F1 делит USART3; здесь USART3 в карту
// не включён, потому что одна нога не может быть двумя функциями.
// ---------------------------------------------------------------------------
#pragma once

#include "boards/pin_map.hpp"

namespace board {

inline constexpr PinMap<20> kPins = PinMap<20>{{
    // USART1
    {Pin{Port::A, 9u},  Signal::UartTx, 1u, 7u, 0u},
    {Pin{Port::A, 10u}, Signal::UartRx, 1u, 7u, 0u},
    // USART2
    {Pin{Port::A, 2u},  Signal::UartTx, 2u, 7u, 0u},
    {Pin{Port::A, 3u},  Signal::UartRx, 2u, 7u, 0u},
    // SPI1
    {Pin{Port::A, 5u},  Signal::SpiSck,  1u, 5u, 0u},
    {Pin{Port::A, 6u},  Signal::SpiMiso, 1u, 5u, 0u},
    {Pin{Port::A, 7u},  Signal::SpiMosi, 1u, 5u, 0u},
    {Pin{Port::A, 4u},  Signal::SpiCs,   1u, 0u, 0u},
    // SPI2
    {Pin{Port::B, 13u}, Signal::SpiSck,  2u, 5u, 0u},
    {Pin{Port::B, 14u}, Signal::SpiMiso, 2u, 5u, 0u},
    {Pin{Port::B, 15u}, Signal::SpiMosi, 2u, 5u, 0u},
    {Pin{Port::B, 12u}, Signal::SpiCs,   2u, 0u, 0u},
    // I2C1
    {Pin{Port::B, 6u},  Signal::I2cScl,  1u, 4u, 0u},
    {Pin{Port::B, 7u},  Signal::I2cSda,  1u, 4u, 0u},
    // I2C2
    {Pin{Port::B, 10u}, Signal::I2cScl,  2u, 4u, 0u},
    {Pin{Port::B, 11u}, Signal::I2cSda,  2u, 4u, 0u},
    // PWM TIM2_CH1
    {Pin{Port::A, 0u},  Signal::Pwm,     2u, 1u, 1u},
    // ADC1: IN1 PA1, IN8 PB0
    {Pin{Port::A, 1u},  Signal::Adc,     1u, 0u, 1u},
    {Pin{Port::B, 0u},  Signal::Adc,     1u, 0u, 8u},
    // LED
    {Pin{Port::C, 13u}, Signal::Led,     0u, 0u, 0u},
}};

static_assert(kPins.valid(), "f407_generic: конфликт или незаданный пин в карте");

} // namespace board
