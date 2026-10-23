// os/rust/rust_bridge.cpp
#include "rust_bridge.hpp"

#include "../config/time_config.hpp"
#include "../core/task/sheduler.hpp"
#include "../iplatform.hpp"
#include "../../hal/IGpio.hpp"
#include "../../hal/ISpi.hpp"
#include "../../hal/II2c.hpp"
#include "../../hal/INvs.hpp"
#include "../net/socket.hpp"
#include "../usb/usb.hpp"

extern "C" void mcframe_rust_main(void) __attribute__((weak));

namespace {

template <typename T, embed::Size N>
struct Registry {
    T* items[N]{};
    uint8_t count = 0u;
    int add(T* item) noexcept {
        if (item == nullptr) return -1;
        for (uint8_t i = 0u; i < N; ++i) {
            if (items[i] == nullptr) { items[i] = item; return i; }
        }
        return -1;
    }
    T* get(int h) noexcept {
        return (h >= 0 && h < static_cast<int>(N)) ? items[h] : nullptr;
    }
    void remove(int h) noexcept {
        if (h >= 0 && h < static_cast<int>(N)) items[h] = nullptr;
    }
};

Registry<embed::hal::IGpioPin, 8> g_gpio;
Registry<embed::hal::ISpi, 4>     g_spi;
Registry<embed::hal::II2c, 4>     g_i2c;
Registry<embed::hal::INvs, 4>     g_nvs;
Registry<embed::net::Factory, 2> g_net;
Registry<embed::net::Socket, 8>  g_sock;
Registry<embed::usb::Hal, 2>     g_usb;

} // namespace

extern "C" {

/// Записать строку из Rust. Строка обязана быть NUL-terminated.
void mc_log(const char* text) noexcept {
    embed::platform_log_line((text == nullptr) ? "" : text);
}

/// Спать из задачи Rust. Вне запущенного планировщика — Unavailable.
int mc_sleep_ms(uint32_t ms) noexcept {
    if (!embed::scheduler::instance().started()) return static_cast<int>(embed::Status::Unavailable);
    return static_cast<int>(embed::scheduler::instance().sleep_for_ms(ms));
}

int mc_yield(void) noexcept {
    if (!embed::scheduler::instance().started()) return static_cast<int>(embed::Status::Unavailable);
    embed::scheduler::instance().yield();
    return static_cast<int>(embed::Status::Ok);
}

uint64_t mc_uptime_ms(void) noexcept {
    return embed::config::OsTimer::NowMs();
}

uint64_t mc_epoch_seconds(void) noexcept {
    return embed::config::OsTimer::NowEpochSeconds();
}

int mc_gpio_register(void* pin) noexcept { return g_gpio.add(static_cast<embed::hal::IGpioPin*>(pin)); }
int mc_gpio_configure(int h, uint8_t mode, uint8_t pull, bool initial) noexcept {
    auto* p = g_gpio.get(h); if (p == nullptr || mode > 3u || pull > 2u) return -1;
    embed::hal::GpioConfig c{};
    c.mode = static_cast<embed::hal::GpioMode>(mode);
    c.pull = static_cast<embed::hal::GpioPull>(pull);
    c.initial = initial;
    return static_cast<int>(p->configure(c));
}
void mc_gpio_set(int h, bool level) noexcept { if (auto* p = g_gpio.get(h)) p->set(level); }
int mc_gpio_get(int h) noexcept { auto* p = g_gpio.get(h); return p ? (p->get() ? 1 : 0) : -1; }

int mc_spi_register(void* bus) noexcept { return g_spi.add(static_cast<embed::hal::ISpi*>(bus)); }
int mc_spi_transfer(int h, const void* tx, void* rx, uint32_t len, uint32_t timeout_ms) noexcept {
    auto* p = g_spi.get(h); if (p == nullptr) return -1;
    return static_cast<int>(p->transfer(tx, rx, len, static_cast<embed::Timeout>(timeout_ms)));
}

int mc_i2c_register(void* bus) noexcept { return g_i2c.add(static_cast<embed::hal::II2c*>(bus)); }
int mc_i2c_write(int h, uint8_t addr, const void* data, uint32_t len, uint32_t timeout_ms) noexcept {
    auto* p = g_i2c.get(h); if (p == nullptr) return -1;
    return static_cast<int>(p->write(addr, data, len, static_cast<embed::Timeout>(timeout_ms)));
}
int mc_i2c_read(int h, uint8_t addr, void* data, uint32_t cap, uint32_t timeout_ms,
                uint32_t* got) noexcept {
    auto* p = g_i2c.get(h); if (p == nullptr || got == nullptr) return -1;
    embed::Size n = 0u;
    const embed::Status st = p->read(addr, data, cap, n, static_cast<embed::Timeout>(timeout_ms));
    *got = static_cast<uint32_t>(n);
    return static_cast<int>(st);
}

int mc_nvs_register(void* nvs) noexcept { return g_nvs.add(static_cast<embed::hal::INvs*>(nvs)); }
int mc_nvs_read(int h, const char* key, void* data, uint32_t cap, uint32_t* got) noexcept {
    auto* p = g_nvs.get(h); if (p == nullptr || key == nullptr || got == nullptr) return -1;
    embed::Size n = 0u;
    const embed::Status st = p->read(key, data, cap, n);
    *got = static_cast<uint32_t>(n);
    return static_cast<int>(st);
}
int mc_nvs_write(int h, const char* key, const void* data, uint32_t len) noexcept {
    auto* p = g_nvs.get(h); if (p == nullptr || key == nullptr) return -1;
    return static_cast<int>(p->write(key, data, len));
}
int mc_nvs_erase(int h, const char* key) noexcept {
    auto* p = g_nvs.get(h); if (p == nullptr || key == nullptr) return -1;
    return static_cast<int>(p->erase(key));
}

int mc_net_register(void* factory) noexcept {
    return g_net.add(static_cast<embed::net::Factory*>(factory));
}
int mc_net_socket(int fh, uint8_t proto) noexcept {
    auto* f = g_net.get(fh); if (f == nullptr || proto > 1u) return -1;
    embed::net::Socket* s = nullptr;
    const embed::Status st = f->create(static_cast<embed::net::Protocol>(proto), s);
    if (st != embed::Status::Ok || s == nullptr) return -1;
    const int h = g_sock.add(s);
    if (h < 0) { s->close(); return -1; }
    return h;
}
int mc_net_resolve(int fh, const char* host, uint8_t ip[4]) noexcept {
    auto* f = g_net.get(fh); if (f == nullptr || host == nullptr || ip == nullptr) return -1;
    embed::net::Endpoint ep{};
    const embed::Status st = f->resolve(host, ep);
    if (st != embed::Status::Ok) return static_cast<int>(st);
    for (uint8_t i = 0u; i < 4u; ++i) ip[i] = ep.addr[i];
    return static_cast<int>(st);
}
int mc_net_connect(int sh, const uint8_t ip[4], uint16_t port, uint32_t timeout_ms) noexcept {
    auto* s = g_sock.get(sh); if (s == nullptr || ip == nullptr) return -1;
    embed::net::Endpoint ep{};
    for (uint8_t i = 0u; i < 4u; ++i) ep.addr[i] = ip[i];
    ep.port = port;
    return static_cast<int>(s->connect(ep, timeout_ms));
}
int mc_net_send(int sh, const void* data, uint32_t len, uint32_t* sent) noexcept {
    auto* s = g_sock.get(sh); if (s == nullptr || sent == nullptr) return -1;
    embed::Size n = 0u;
    const embed::Status st = s->send(data, len, n);
    *sent = static_cast<uint32_t>(n);
    return static_cast<int>(st);
}
int mc_net_recv(int sh, void* data, uint32_t cap, uint32_t timeout_ms,
                uint32_t* got) noexcept {
    auto* s = g_sock.get(sh); if (s == nullptr || got == nullptr) return -1;
    embed::Size n = 0u;
    const embed::Status st = s->recv(data, cap, n, timeout_ms);
    *got = static_cast<uint32_t>(n);
    return static_cast<int>(st);
}
void mc_net_close(int sh) noexcept {
    auto* s = g_sock.get(sh);
    if (s != nullptr) { s->close(); g_sock.remove(sh); }
}

int mc_usb_register(void* hal) noexcept { return g_usb.add(static_cast<embed::usb::Hal*>(hal)); }
int mc_usb_write(int h, uint8_t ep, const void* data, uint32_t len) noexcept {
    auto* p = g_usb.get(h); if (p == nullptr) return -1;
    return static_cast<int>(p->ep_write(ep, data, len));
}
int mc_usb_read(int h, uint8_t ep, void* data, uint32_t cap, uint32_t,
                uint32_t* got) noexcept {
    auto* p = g_usb.get(h); if (p == nullptr || got == nullptr) return -1;
    embed::Size n = 0u;
    const embed::Status st = p->ep_read(ep, data, cap, n);
    *got = static_cast<uint32_t>(n);
    return static_cast<int>(st);
}

} // extern "C"

namespace embed::rust {

bool available() noexcept {
    return mcframe_rust_main != nullptr;
}

bool run() noexcept {
    if (mcframe_rust_main == nullptr) return false;
    mcframe_rust_main();
    return true;
}

} // namespace embed::rust
