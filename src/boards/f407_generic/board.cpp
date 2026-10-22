#include "boards/f407_generic/board.hpp"

namespace {

embed::stm32::Uart1 g_console{16000000u};
embed::stm32::GpioPin g_led{GPIOC, 13u};
embed::stm32::GpioPin g_cs{GPIOA, 4u};
embed::stm32::GpioPin g_sck{GPIOA, 5u};
embed::stm32::GpioPin g_miso{GPIOA, 6u};
embed::stm32::GpioPin g_mosi{GPIOA, 7u};
embed::stm32::GpioPin g_pwm_pin{GPIOA, 0u};
embed::stm32::Spi g_spi{SPI1, 16000000u, g_sck, g_miso, g_mosi};
embed::stm32::Pwm g_pwm{TIM2, 1u, g_pwm_pin, 16000000u};

} // namespace

namespace board {

Uart& uart1() noexcept { return g_console; }
Console& console() noexcept { return g_console; }
Gpio& led() noexcept { return g_led; }
Gpio& spi_cs() noexcept { return g_cs; }
Spi& spi1() noexcept { return g_spi; }
Pwm& pwm1() noexcept { return g_pwm; }

void init() noexcept {
    embed::hal::GpioConfig out{};
    out.mode = embed::hal::GpioMode::Output;
    out.initial = false;
    (void)g_led.configure(out);
    embed::hal::GpioConfig cs{};
    cs.mode = embed::hal::GpioMode::Output;
    cs.initial = true;
    (void)g_cs.configure(cs);
}

} // namespace board

extern "C" void EXTI0_IRQHandler(void)  { embed::stm32::exti_dispatch(0u); }
extern "C" void EXTI1_IRQHandler(void)  { embed::stm32::exti_dispatch(1u); }
extern "C" void EXTI2_IRQHandler(void)  { embed::stm32::exti_dispatch(2u); }
extern "C" void EXTI3_IRQHandler(void)  { embed::stm32::exti_dispatch(3u); }
extern "C" void EXTI4_IRQHandler(void)  { embed::stm32::exti_dispatch(4u); }
extern "C" void EXTI9_5_IRQHandler(void) {
    for (uint8_t i = 5u; i <= 9u; ++i) embed::stm32::exti_dispatch(i);
}
extern "C" void EXTI15_10_IRQHandler(void) {
    for (uint8_t i = 10u; i <= 15u; ++i) embed::stm32::exti_dispatch(i);
}
extern "C" void SPI1_IRQHandler(void) { embed::stm32::Spi::dispatch(0u); }
extern "C" void SPI2_IRQHandler(void) { embed::stm32::Spi::dispatch(1u); }
