// os/rust/rust_bridge.hpp
// ---------------------------------------------------------------------------
// C ABI для Rust-приложения. Ресурсы (GPIO/SPI/I2C/NVS/net/USB) регистрирует
// C++-сторона; Rust получает стабильные целочисленные handle и не зависит от
// C++ ABI/указателей.
// ---------------------------------------------------------------------------
#pragma once

#include "../common.hpp"

namespace embed::rust {
bool run() noexcept;
bool available() noexcept;
} // namespace embed::rust

extern "C" {
uint64_t mc_uptime_ms(void) noexcept;
uint64_t mc_epoch_seconds(void) noexcept;

/// GPIO: register принимает hal::IGpioPin*, возвращает handle или -1.
int mc_gpio_register(void* pin) noexcept;
int mc_gpio_configure(int handle, uint8_t mode, uint8_t pull, bool initial) noexcept;
void mc_gpio_set(int handle, bool level) noexcept;
int mc_gpio_get(int handle) noexcept;   // 0/1, -1 — ошибка

/// SPI: hal::ISpi*.
int mc_spi_register(void* bus) noexcept;
int mc_spi_transfer(int handle, const void* tx, void* rx, uint32_t len,
                    uint32_t timeout_ms) noexcept;

/// I2C: hal::II2c*.
int mc_i2c_register(void* bus) noexcept;
int mc_i2c_write(int handle, uint8_t address, const void* data, uint32_t len,
                 uint32_t timeout_ms) noexcept;
int mc_i2c_read(int handle, uint8_t address, void* data, uint32_t cap,
                uint32_t timeout_ms, uint32_t* got) noexcept;

/// NVS: hal::INvs*.
int mc_nvs_register(void* nvs) noexcept;
int mc_nvs_read(int handle, const char* key, void* data, uint32_t cap,
                uint32_t* got) noexcept;
int mc_nvs_write(int handle, const char* key, const void* data, uint32_t len) noexcept;
int mc_nvs_erase(int handle, const char* key) noexcept;

/// net::Factory* / net::Socket*.
int mc_net_register(void* factory) noexcept;
int mc_net_socket(int factory, uint8_t proto) noexcept;   // 0 TCP, 1 UDP
int mc_net_resolve(int factory, const char* host, uint8_t ip[4]) noexcept;
int mc_net_connect(int socket, const uint8_t ip[4], uint16_t port,
                   uint32_t timeout_ms) noexcept;
int mc_net_send(int socket, const void* data, uint32_t len,
                uint32_t* sent) noexcept;
int mc_net_recv(int socket, void* data, uint32_t cap,
                uint32_t timeout_ms, uint32_t* got) noexcept;
void mc_net_close(int socket) noexcept;

/// USB: usb::Hal*.
int mc_usb_register(void* hal) noexcept;
int mc_usb_write(int handle, uint8_t ep, const void* data, uint32_t len) noexcept;
int mc_usb_read(int handle, uint8_t ep, void* data, uint32_t cap,
                uint32_t timeout_ms, uint32_t* got) noexcept;
}
