// boards/f103_generic/board.cpp
#include "boards/f103_generic/board.hpp"
#include "os/nvs/nvs.hpp"

#ifndef MCFRAME_NVS_SIZE
#define MCFRAME_NVS_SIZE 0u
#endif
#ifndef MCFRAME_NVS_OFFSET
#define MCFRAME_NVS_OFFSET 0u
#endif

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

// NVS: внутренняя область в конце Flash (flat-образ) либо внешнее
// хранилище, которое приложение подключает через hal::INvs. Для slot-образа
// на 128 КБ внутренней области нет — MCFRAME_NVS_SIZE=0.
embed::nvs::HalNvs g_nvs;
#if MCFRAME_NVS_SIZE > 0
embed::ota::InternalFlashStorage g_nvs_flash{MCFRAME_NVS_OFFSET, MCFRAME_NVS_SIZE};
embed::nvs::Store                g_nvs_store;
#endif

embed::stm32::GpioPin g_led{GPIOC, 13u};
embed::stm32::GpioPin g_cs{GPIOA, 4u};
embed::stm32::GpioPin g_sck{GPIOA, 5u};
embed::stm32::GpioPin g_miso{GPIOA, 6u};
embed::stm32::GpioPin g_mosi{GPIOA, 7u};
embed::stm32::GpioPin g_cs2{GPIOB, 12u};
embed::stm32::GpioPin g_sck2{GPIOB, 13u};
embed::stm32::GpioPin g_miso2{GPIOB, 14u};
embed::stm32::GpioPin g_mosi2{GPIOB, 15u};
embed::stm32::GpioPin g_pwm_pin{GPIOA, 0u};

embed::stm32::Spi g_spi{SPI1, 72000000u, g_sck, g_miso, g_mosi};
embed::stm32::Spi g_spi2{SPI2, 36000000u, g_sck2, g_miso2, g_mosi2};
embed::stm32::Pwm g_pwm{TIM2, 1u, g_pwm_pin, 72000000u};

} // namespace

namespace board {

Uart&  uart1() noexcept { return Uart::instance(); }
Uart2& uart2() noexcept { return Uart2::instance(); }
Uart3& uart3() noexcept { return Uart3::instance(); }
Nvs&   nvs() noexcept { return g_nvs; }
Console& console() noexcept { return g_console; }
Gpio& led() noexcept { return g_led; }
Gpio& spi_cs() noexcept { return g_cs; }
Gpio& spi2_cs() noexcept { return g_cs2; }
Spi& spi1() noexcept { return g_spi; }
Spi& spi2() noexcept { return g_spi2; }
Pwm& pwm1() noexcept { return g_pwm; }

void init() noexcept {
    // Пины UART — из карты платы. На F1 альтернативная функция задаётся
    // режимом GPIO (AF push-pull), а не полем AFR: TX — Alternate, RX —
    // вход. Драйвер USART не трогает GPIO, чтобы не завязываться на плату.
    auto uart_pins = [](board::Signal sig, uint8_t instance, embed::hal::GpioMode mode) {
        const board::Pin pin = board::kPins.pin(sig, instance);
        if (!pin.valid()) return;
        embed::stm32::GpioPin gpio{pin};
        embed::hal::GpioConfig cfg{};
        cfg.mode = mode;
        cfg.pull = embed::hal::GpioPull::Up;
        (void)gpio.configure(cfg);
    };
    for (uint8_t i = 1u; i <= 3u; ++i) {
        uart_pins(board::Signal::UartTx, i, embed::hal::GpioMode::Alternate);
        uart_pins(board::Signal::UartRx, i, embed::hal::GpioMode::Input);
    }

#if MCFRAME_NVS_SIZE > 0
    if (g_nvs_store.init({&g_nvs_flash, MCFRAME_NVS_SIZE / 2u}) == embed::Status::Ok) {
        g_nvs.bind(&g_nvs_store);
    }
#endif
    embed::hal::GpioConfig cfg{};
    cfg.mode = embed::hal::GpioMode::Output;
    cfg.initial = false;
    (void)g_led.configure(cfg);
    (void)g_cs.configure(cfg);
    (void)g_cs2.configure(cfg);
    g_cs.set(true);   // CS неактивен (active low)
    g_cs2.set(true);
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
