// boards/mock_generic/board.cpp
#include "boards/mock_generic/board.hpp"

namespace {

embed::hal::mock::MockUart    g_console;
embed::hal::mock::MockGpioPin g_led;
embed::hal::mock::MockSpi     g_spi;
embed::hal::mock::MockPwm     g_pwm;

} // namespace

namespace board {

Uart& uart1() noexcept { return Uart::instance(); }
Console& console() noexcept { return g_console; }
Gpio& led() noexcept { return g_led; }
Spi& spi1() noexcept { return g_spi; }
Pwm& pwm1() noexcept { return g_pwm; }

void init() noexcept {
    embed::hal::GpioConfig cfg{};
    cfg.mode = embed::hal::GpioMode::Output;
    cfg.initial = false;
    (void)g_led.configure(cfg);
    (void)g_console.configure(embed::hal::UartConfig{});
    (void)g_console.open();
}

} // namespace board
