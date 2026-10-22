// boards/f103_generic/board.cpp
#include "boards/f103_generic/board.hpp"

namespace {

using HalStatus = embed::Status;

class BoardUart final : public embed::hal::IUart {
public:
    explicit BoardUart(board::Uart& uart) noexcept : uart_(uart) {}

    HalStatus configure(const embed::hal::UartConfig& cfg) noexcept override {
        board::Uart::Config c{};
        c.baudRate = cfg.baud_rate;
        c.dataBits = cfg.data_bits;
        c.flowControl = cfg.flow_control;
        switch (cfg.parity) {
            case embed::hal::UartParity::None: c.parity = board::Uart::Parity::None; break;
            case embed::hal::UartParity::Odd:  c.parity = board::Uart::Parity::Odd;  break;
            case embed::hal::UartParity::Even: c.parity = board::Uart::Parity::Even; break;
        }
        c.stopBits = (cfg.stop_bits == embed::hal::UartStopBits::Two)
                   ? board::Uart::StopBits::Two
                   : board::Uart::StopBits::One;
        return uart_.configure(c);
    }

    HalStatus open() noexcept override  { return uart_.open(); }
    HalStatus close() noexcept override { return uart_.close(); }

    HalStatus read(void* buffer, embed::Size capacity, embed::Size& bytes_read,
                   embed::Timeout timeout) noexcept override {
        return uart_.read(buffer, capacity, bytes_read, timeout);
    }
    HalStatus write(const void* data, embed::Size len, embed::Size& written,
                    embed::Timeout timeout) noexcept override {
        return uart_.write(data, len, written, timeout);
    }

private:
    board::Uart& uart_;
};

BoardUart      g_console{board::Uart::instance()};

embed::stm32::GpioPin g_led{GPIOC, 13u};
embed::stm32::GpioPin g_cs{GPIOA, 4u};
embed::stm32::GpioPin g_sck{GPIOA, 5u};
embed::stm32::GpioPin g_miso{GPIOA, 6u};
embed::stm32::GpioPin g_mosi{GPIOA, 7u};
embed::stm32::GpioPin g_pwm_pin{GPIOA, 0u};

embed::stm32::Spi g_spi{SPI1, 72000000u, g_sck, g_miso, g_mosi};
embed::stm32::Pwm g_pwm{TIM2, 1u, g_pwm_pin, 72000000u};

} // namespace

namespace board {

Uart& uart1() noexcept { return Uart::instance(); }
Console& console() noexcept { return g_console; }
Gpio& led() noexcept { return g_led; }
Gpio& spi_cs() noexcept { return g_cs; }
Spi& spi1() noexcept { return g_spi; }
Pwm& pwm1() noexcept { return g_pwm; }

void init() noexcept {
    embed::hal::GpioConfig cfg{};
    cfg.mode = embed::hal::GpioMode::Output;
    cfg.initial = false;
    (void)g_led.configure(cfg);
    (void)g_cs.configure(cfg);
    g_cs.set(true);   // CS неактивен (active low)
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
