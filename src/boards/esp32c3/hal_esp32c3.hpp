// boards/esp32c3/hal_esp32c3.hpp
// ---------------------------------------------------------------------------
// HAL для ESP32-C3 поверх ESP-IDF drivers.
// ---------------------------------------------------------------------------
#pragma once

#include "hal/IGpio.hpp"
#include "hal/ISpi.hpp"
#include "hal/IPwm.hpp"
#include "hal/IUart.hpp"
#include "hal/INvs.hpp"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "nvs.h"

namespace embed::esp32c3 {

class GpioPin final : public hal::IGpioPin {
public:
    GpioPin(gpio_num_t pin) noexcept : pin_(pin) {}
    Status configure(const hal::GpioConfig& cfg) noexcept override;
    void set(bool level) noexcept override { gpio_set_level(pin_, level ? 1 : 0); }
    bool get() const noexcept override { return gpio_get_level(pin_) != 0; }
    void toggle() noexcept override { set(!get()); }
    Status attach_irq(hal::GpioEdge edge, hal::GpioIrqCallback cb, void* ctx) noexcept override;
    void detach_irq() noexcept override;
    static void isr_entry(void* arg) noexcept;
private:
    gpio_num_t pin_;
    hal::GpioIrqCallback cb_ = nullptr;
    void* ctx_ = nullptr;
};

class Uart final : public hal::IUart {
public:
    explicit Uart(uart_port_t port) noexcept : port_(port) {}
    Status configure(const hal::UartConfig& cfg) noexcept override;
    Status open() noexcept override;
    Status close() noexcept override;
    Status read(void* buffer, Size capacity, Size& bytes_read, Timeout timeout) noexcept override;
    Status write(const void* data, Size len, Size& written, Timeout timeout) noexcept override;
private:
    uart_port_t port_;
    bool installed_ = false;
};

class Spi final : public hal::ISpi {
public:
    Spi(gpio_num_t mosi, gpio_num_t miso, gpio_num_t sclk) noexcept
        : mosi_(mosi), miso_(miso), sclk_(sclk) {}
    Status configure(const hal::SpiConfig& cfg) noexcept override;
    Status transfer(const void* tx, void* rx, Size n, Timeout timeout) noexcept override;
    Status lock(Timeout timeout) noexcept override;
    void unlock() noexcept override;
    uint32_t caps() const noexcept override { return hal::SpiCapCs; }
private:
    gpio_num_t mosi_, miso_, sclk_;
    spi_device_handle_t dev_ = nullptr;
    bool busy_ = false;
};

class Pwm final : public hal::IPwm {
public:
    explicit Pwm(gpio_num_t pin) noexcept : pin_(pin) {}
    Status configure(const hal::PwmConfig& cfg) noexcept override;
    Status set_duty(uint8_t duty) noexcept override;
    Status start() noexcept override;
    Status stop() noexcept override;
    uint32_t caps() const noexcept override { return 1u; }
private:
    gpio_num_t pin_;
    uint32_t freq_ = 1000u;
    uint8_t duty_ = 0u;
    ledc_channel_t channel_ = LEDC_CHANNEL_0;
};

class Nvs final : public hal::INvs {
public:
    Status init() noexcept override;
    Status read(const char* key, void* buffer, Size capacity, Size& len) noexcept override;
    Status write(const char* key, const void* data, Size len) noexcept override;
    Status erase(const char* key) noexcept override;
private:
    nvs_handle_t handle_ = 0;
    bool opened_ = false;
};

} // namespace embed::esp32c3
