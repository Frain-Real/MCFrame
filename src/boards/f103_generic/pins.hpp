// boards/f103_generic/pins.hpp
// ---------------------------------------------------------------------------
// Карта пинов generic-платы STM32F103 (Blue Pill-подобная разводка).
//
// Карта — единственный источник истины: BSP создаёт драйверы ИЗ неё, а
// static_assert ниже не даёт собрать плату с двумя функциями на одной ноге.
//
// Занятые функции:
//   USART1 PA9/PA10, USART2 PA2/PA3, USART3 PB10/PB11;
//   SPI1 PA5/6/7 + CS PA4, SPI2 PB13/14/15 + CS PB12;
//   I2C1 PB6/PB7; PWM TIM2_CH1 PA0; ADC1 IN1 PA1, IN8 PB0; LED PC13.
//
// I2C2 (PB10/PB11) намеренно НЕ объявлен: он делит ноги с USART3, и карта
// должна выбирать одно из двух, а не делать вид, что доступны оба.
// ---------------------------------------------------------------------------
#pragma once

#include "boards/pin_map.hpp"

namespace board {

inline constexpr PinMap<20> kPins = PinMap<20>{{
    // USART1 (APB2)
    {Pin{Port::A, 9u},  Signal::UartTx, 1u, 0u, 0u},
    {Pin{Port::A, 10u}, Signal::UartRx, 1u, 0u, 0u},
    // USART2 (APB1)
    {Pin{Port::A, 2u},  Signal::UartTx, 2u, 0u, 0u},
    {Pin{Port::A, 3u},  Signal::UartRx, 2u, 0u, 0u},
    // USART3 (APB1)
    {Pin{Port::B, 10u}, Signal::UartTx, 3u, 0u, 0u},
    {Pin{Port::B, 11u}, Signal::UartRx, 3u, 0u, 0u},
    // SPI1
    {Pin{Port::A, 5u},  Signal::SpiSck,  1u, 0u, 0u},
    {Pin{Port::A, 6u},  Signal::SpiMiso, 1u, 0u, 0u},
    {Pin{Port::A, 7u},  Signal::SpiMosi, 1u, 0u, 0u},
    {Pin{Port::A, 4u},  Signal::SpiCs,   1u, 0u, 0u},
    // SPI2
    {Pin{Port::B, 13u}, Signal::SpiSck,  2u, 0u, 0u},
    {Pin{Port::B, 14u}, Signal::SpiMiso, 2u, 0u, 0u},
    {Pin{Port::B, 15u}, Signal::SpiMosi, 2u, 0u, 0u},
    {Pin{Port::B, 12u}, Signal::SpiCs,   2u, 0u, 0u},
    // I2C1
    {Pin{Port::B, 6u},  Signal::I2cScl,  1u, 0u, 0u},
    {Pin{Port::B, 7u},  Signal::I2cSda,  1u, 0u, 0u},
    // PWM TIM2_CH1
    {Pin{Port::A, 0u},  Signal::Pwm,     2u, 0u, 1u},
    // ADC1: IN1 PA1, IN8 PB0
    {Pin{Port::A, 1u},  Signal::Adc,     1u, 0u, 1u},
    {Pin{Port::B, 0u},  Signal::Adc,     1u, 0u, 8u},
    // LED
    {Pin{Port::C, 13u}, Signal::Led,     0u, 0u, 0u},
}};

static_assert(kPins.valid(), "f103_generic: конфликт или незаданный пин в карте");

} // namespace board
