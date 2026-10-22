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

class BoardLed final : public embed::hal::IGpioPin {
public:
    HalStatus configure(const embed::hal::GpioConfig& cfg) noexcept override {
        if (cfg.mode != embed::hal::GpioMode::Output) return HalStatus::NotSupported;
        // PC13: выход push-pull 2 МГц. В CRH биты 20..23.
        RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
        constexpr uint32_t kMask = 0xFu << 20;
        GPIOC->CRH = (GPIOC->CRH & ~kMask) | (0x2u << 20);
        level_ = cfg.initial;
        set(level_);
        return HalStatus::Ok;
    }

    void set(bool level) noexcept override {
        level_ = level;
        // LED на PC13 активен низким уровнем.
        if (level_) GPIOC->BRR = (1u << 13);
        else        GPIOC->BSRR = (1u << 13);
    }
    bool get() const noexcept override { return level_; }
    void toggle() noexcept override { set(!level_); }

    HalStatus attach_irq(embed::hal::GpioEdge, embed::hal::GpioIrqCallback,
                         void*) noexcept override {
        return HalStatus::NotSupported;
    }
    void detach_irq() noexcept override {}

private:
    bool level_ = false;
};

class UnsupportedSpi final : public embed::hal::ISpi {
public:
    HalStatus configure(const embed::hal::SpiConfig&) noexcept override {
        return HalStatus::NotSupported;
    }
    HalStatus transfer(const void*, void*, embed::Size, embed::Timeout) noexcept override {
        return HalStatus::NotSupported;
    }
    HalStatus lock(embed::Timeout) noexcept override { return HalStatus::NotSupported; }
    void unlock() noexcept override {}
    uint32_t caps() const noexcept override { return 0u; }
};

class UnsupportedPwm final : public embed::hal::IPwm {
public:
    HalStatus configure(const embed::hal::PwmConfig&) noexcept override {
        return HalStatus::NotSupported;
    }
    HalStatus set_duty(uint8_t) noexcept override { return HalStatus::NotSupported; }
    HalStatus start() noexcept override { return HalStatus::NotSupported; }
    HalStatus stop() noexcept override { return HalStatus::NotSupported; }
    uint32_t caps() const noexcept override { return 0u; }
};

BoardUart      g_console{board::Uart::instance()};
BoardLed       g_led;
UnsupportedSpi g_spi;
UnsupportedPwm g_pwm;

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
}

} // namespace board
