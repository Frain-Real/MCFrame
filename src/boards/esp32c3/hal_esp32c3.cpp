#include "boards/esp32c3/hal_esp32c3.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace embed::esp32c3 {

namespace {
portMUX_TYPE g_spi_mux = portMUX_INITIALIZER_UNLOCKED;
} // namespace

Status GpioPin::configure(const hal::GpioConfig& cfg) noexcept {
    gpio_config_t c{};
    c.pin_bit_mask = (1ULL << pin_);
    c.mode = (cfg.mode == hal::GpioMode::Output)
           ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
    c.pull_up_en = (cfg.pull == hal::GpioPull::Up) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    c.pull_down_en = (cfg.pull == hal::GpioPull::Down) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    c.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&c) != ESP_OK) return Status::HardwareError;
    if (cfg.mode == hal::GpioMode::Output) set(cfg.initial);
    return Status::Ok;
}

void GpioPin::isr_entry(void* arg) noexcept {
    auto* self = static_cast<GpioPin*>(arg);
    if (self != nullptr && self->cb_ != nullptr) self->cb_(self->ctx_);
}

Status GpioPin::attach_irq(hal::GpioEdge edge, hal::GpioIrqCallback cb, void* ctx) noexcept {
    if (cb == nullptr) return Status::InvalidArg;
    cb_ = cb;
    ctx_ = ctx;
    gpio_set_intr_type(pin_, edge == hal::GpioEdge::Rising ? GPIO_INTR_POSEDGE
                          : edge == hal::GpioEdge::Falling ? GPIO_INTR_NEGEDGE
                          : GPIO_INTR_ANYEDGE);
    if (gpio_install_isr_service(0) != ESP_OK) {
        // уже установлен — это нормально
    }
    if (gpio_isr_handler_add(pin_, &GpioPin::isr_entry, this) != ESP_OK) {
        return Status::HardwareError;
    }
    return gpio_intr_enable(pin_) == ESP_OK ? Status::Ok : Status::HardwareError;
}

void GpioPin::detach_irq() noexcept {
    (void)gpio_intr_disable(pin_);
    (void)gpio_isr_handler_remove(pin_);
    cb_ = nullptr;
    ctx_ = nullptr;
}

Status Uart::configure(const hal::UartConfig& cfg) noexcept {
    uart_config_t c{};
    c.baud_rate = static_cast<int>(cfg.baud_rate);
    c.data_bits = UART_DATA_8_BITS;
    c.parity = cfg.parity == hal::UartParity::Even ? UART_PARITY_EVEN
             : cfg.parity == hal::UartParity::Odd ? UART_PARITY_ODD
             : UART_PARITY_DISABLE;
    c.stop_bits = cfg.stop_bits == hal::UartStopBits::Two ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
    c.flow_ctrl = cfg.flow_control ? UART_HW_FLOWCTRL_CTS_RTS : UART_HW_FLOWCTRL_DISABLE;
    c.source_clk = UART_SCLK_DEFAULT;
    if (uart_param_config(port_, &c) != ESP_OK) return Status::HardwareError;
    if (!installed_) {
        if (uart_driver_install(port_, 1024, 0, 0, nullptr, 0) != ESP_OK) {
            return Status::HardwareError;
        }
        installed_ = true;
    }
    return Status::Ok;
}
Status Uart::open() noexcept { return Status::Ok; }
Status Uart::close() noexcept {
    if (installed_) { (void)uart_driver_delete(port_); installed_ = false; }
    return Status::Ok;
}
Status Uart::read(void* buffer, Size capacity, Size& bytes_read, Timeout timeout) noexcept {
    bytes_read = 0u;
    if (buffer == nullptr || capacity == 0u) return Status::InvalidArg;
    const TickType_t t = (timeout == 0u) ? portMAX_DELAY : pdMS_TO_TICKS(timeout);
    const int n = uart_read_bytes(port_, buffer, capacity, t);
    if (n < 0) return Status::HardwareError;
    if (n == 0) return Status::Timeout;
    bytes_read = static_cast<Size>(n);
    return Status::Ok;
}
Status Uart::write(const void* data, Size len, Size& written, Timeout) noexcept {
    written = 0u;
    if (data == nullptr || len == 0u) return Status::InvalidArg;
    const int n = uart_write_bytes(port_, data, len);
    if (n < 0) return Status::HardwareError;
    written = static_cast<Size>(n);
    return Status::Ok;
}

Status Spi::configure(const hal::SpiConfig& cfg) noexcept {
    spi_bus_config_t bus{};
    bus.mosi_io_num = mosi_;
    bus.miso_io_num = miso_;
    bus.sclk_io_num = sclk_;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    const esp_err_t st = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (st != ESP_OK && st != ESP_ERR_INVALID_STATE) return Status::HardwareError;

    spi_device_interface_config_t dev{};
    dev.clock_speed_hz = static_cast<int>(cfg.hz);
    dev.mode = cfg.mode;
    dev.spics_io_num = -1;
    dev.queue_size = 1;
    if (dev_ == nullptr && spi_bus_add_device(SPI2_HOST, &dev, &dev_) != ESP_OK) {
        return Status::HardwareError;
    }
    return Status::Ok;
}

Status Spi::transfer(const void* tx, void* rx, Size n, Timeout) noexcept {
    if (dev_ == nullptr || n == 0u || (tx == nullptr && rx == nullptr)) {
        return Status::InvalidArg;
    }
    spi_transaction_t t{};
    t.length = n * 8u;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    return spi_device_transmit(dev_, &t) == ESP_OK ? Status::Ok : Status::HardwareError;
}

Status Spi::lock(Timeout) noexcept {
    taskENTER_CRITICAL(&g_spi_mux);
    const bool busy = busy_;
    if (!busy) busy_ = true;
    taskEXIT_CRITICAL(&g_spi_mux);
    return busy ? Status::Busy : Status::Ok;
}
void Spi::unlock() noexcept {
    taskENTER_CRITICAL(&g_spi_mux);
    busy_ = false;
    taskEXIT_CRITICAL(&g_spi_mux);
}

Status Pwm::configure(const hal::PwmConfig& cfg) noexcept {
    if (cfg.frequency_hz == 0u) return Status::InvalidArg;
    freq_ = cfg.frequency_hz;
    duty_ = cfg.duty_percent > 100u ? 100u : cfg.duty_percent;
    ledc_timer_config_t t{};
    t.speed_mode = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_10_BIT;
    t.timer_num = LEDC_TIMER_0;
    t.freq_hz = freq_;
    t.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&t) != ESP_OK) return Status::HardwareError;

    ledc_channel_config_t ch{};
    ch.gpio_num = pin_;
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.channel = channel_;
    ch.intr_type = LEDC_INTR_DISABLE;
    ch.timer_sel = LEDC_TIMER_0;
    ch.duty = (1024u * duty_) / 100u;
    ch.hpoint = 0;
    if (ledc_channel_config(&ch) != ESP_OK) return Status::HardwareError;
    return Status::Ok;
}
Status Pwm::set_duty(uint8_t duty) noexcept {
    if (duty > 100u) return Status::InvalidArg;
    duty_ = duty;
    const uint32_t d = (1024u * duty) / 100u;
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, channel_, d) != ESP_OK) return Status::HardwareError;
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, channel_) == ESP_OK ? Status::Ok : Status::HardwareError;
}
Status Pwm::start() noexcept { return set_duty(duty_); }
Status Pwm::stop() noexcept { return set_duty(0u); }

Status Nvs::init() noexcept {
    if (opened_) return Status::Ok;
    return nvs_open("mcframe", NVS_READWRITE, &handle_) == ESP_OK
        ? (opened_ = true, Status::Ok) : Status::HardwareError;
}
Status Nvs::read(const char* key, void* buffer, Size capacity, Size& len) noexcept {
    len = 0u;
    if (!opened_ || key == nullptr || buffer == nullptr) return Status::InvalidArg;
    size_t sz = capacity;
    if (nvs_get_blob(handle_, key, buffer, &sz) != ESP_OK) return Status::NotFound;
    len = static_cast<Size>(sz);
    return Status::Ok;
}
Status Nvs::write(const char* key, const void* data, Size len) noexcept {
    if (!opened_ || key == nullptr || data == nullptr) return Status::InvalidArg;
    if (nvs_set_blob(handle_, key, data, len) != ESP_OK) return Status::HardwareError;
    return nvs_commit(handle_) == ESP_OK ? Status::Ok : Status::HardwareError;
}
Status Nvs::erase(const char* key) noexcept {
    if (!opened_ || key == nullptr) return Status::InvalidArg;
    if (nvs_erase_key(handle_, key) != ESP_OK) return Status::NotFound;
    return nvs_commit(handle_) == ESP_OK ? Status::Ok : Status::HardwareError;
}

} // namespace embed::esp32c3
