// platforms/stm32/exti_stm32.hpp
#pragma once
#include "stm32_regs.hpp"
#include "hal/IGpio.hpp"

namespace embed::stm32 {
Status exti_attach(GPIO_TypeDef* port, uint8_t pin, hal::GpioEdge edge,
                   hal::GpioIrqCallback cb, void* ctx) noexcept;
void exti_detach(uint8_t pin) noexcept;
void exti_dispatch(uint8_t line) noexcept;
} // namespace embed::stm32
