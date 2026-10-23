// tests/test_rust_bridge.cpp — host smoke-test C ABI Rust <-> C++ и FFI.
#include "os/rust/rust_bridge.hpp"
#include "hal/mock/mock_hal.hpp"
#include "os/net/socket.hpp"
#include "os/usb/usb.hpp"

#include <cstdio>
#include <cstring>

extern "C" unsigned int mcframe_rust_selfcheck(void);

namespace {

class TestSocket final : public embed::net::Socket {
public:
    embed::Status connect(const embed::net::Endpoint&, uint32_t) noexcept override {
        open_ = true; return embed::Status::Ok;
    }
    embed::Status send(const void* d, embed::Size n, embed::Size& sent) noexcept override {
        const auto* p = static_cast<const uint8_t*>(d);
        for (embed::Size i = 0u; i < n && tx_len_ < sizeof(tx_); ++i) tx_[tx_len_++] = p[i];
        sent = n; return embed::Status::Ok;
    }
    embed::Status recv(void* d, embed::Size cap, embed::Size& got, uint32_t) noexcept override {
        auto* p = static_cast<uint8_t*>(d);
        const embed::Size n = (cap < 2u) ? cap : 2u;
        for (embed::Size i = 0u; i < n; ++i) p[i] = static_cast<uint8_t>('A' + i);
        got = n; return embed::Status::Ok;
    }
    embed::Status send_to(const embed::net::Endpoint&, const void* d, embed::Size n,
                          embed::Size& sent) noexcept override {
        return send(d, n, sent);
    }
    embed::Status recv_from(embed::net::Endpoint&, void* d, embed::Size cap,
                            embed::Size& got, uint32_t) noexcept override {
        return recv(d, cap, got, 0u);
    }
    void close() noexcept override { open_ = false; }
    bool open() const noexcept override { return open_; }
    embed::Size tx_len() const noexcept { return tx_len_; }
    const uint8_t* tx() const noexcept { return tx_; }
private:
    uint8_t tx_[32]{};
    embed::Size tx_len_ = 0u;
    bool open_ = false;
};

class TestFactory final : public embed::net::Factory {
public:
    embed::Status create(embed::net::Protocol, embed::net::Socket*& out) noexcept override {
        out = &socket; socket.close(); return embed::Status::Ok;
    }
    embed::Status resolve(const char*, embed::net::Endpoint& out) noexcept override {
        out = embed::net::Endpoint{127, 0, 0, 1, 80};
        return embed::Status::Ok;
    }
    TestSocket socket;
};

class TestUsbHal final : public embed::usb::Hal {
public:
    embed::Status ep0_write(const void*, embed::Size) noexcept override { return embed::Status::Ok; }
    embed::Status ep0_read(void*, embed::Size, embed::Size& got) noexcept override {
        got = 0u; return embed::Status::NotSupported;
    }
    embed::Status ep_write(uint8_t ep, const void* d, embed::Size n) noexcept override {
        ep_ = ep; len_ = n;
        if (n <= sizeof(buf_)) std::memcpy(buf_, d, n);
        return embed::Status::Ok;
    }
    embed::Status ep_read(uint8_t, void* d, embed::Size cap, embed::Size& got) noexcept override {
        got = (cap < 2u) ? cap : 2u;
        auto* p = static_cast<uint8_t*>(d);
        for (embed::Size i = 0u; i < got; ++i) p[i] = static_cast<uint8_t>('U' + i);
        return embed::Status::Ok;
    }
    void stall_ep0() noexcept override {}
    uint8_t ep_ = 0u;
    embed::Size len_ = 0u;
    uint8_t buf_[64]{};
};

} // namespace

int main() {
    using namespace embed;
    hal::mock::MockGpioPin pin;
    hal::mock::MockSpi spi;
    (void)spi.configure(hal::SpiConfig{});
    const int gh = mc_gpio_register(&pin);
    const int sh = mc_spi_register(&spi);
    bool gpio_ok = gh >= 0 && mc_gpio_configure(gh, 1u, 0u, true) == 0 &&
                         mc_gpio_get(gh) == 1;
    mc_gpio_set(gh, true);
    gpio_ok = gpio_ok && mc_gpio_get(gh) == 1;
    uint8_t tx[2] = {1u, 2u};
    uint8_t rx[2]{};
    const bool spi_ok = sh >= 0 && mc_spi_transfer(sh, tx, rx, 2u, 100u) == 0 &&
                        rx[0] == 1u && rx[1] == 2u;

    hal::mock::MockNvs nvs;
    const int nh = mc_nvs_register(&nvs);
    uint8_t nbuf[8]{};
    uint32_t ngot = 0u;
    bool nvs_ok = nh >= 0 && mc_nvs_write(nh, "k", "value", 5u) == 0 &&
                  mc_nvs_read(nh, "k", nbuf, sizeof(nbuf), &ngot) == 0 &&
                  ngot == 5u && std::memcmp(nbuf, "value", 5u) == 0 &&
                  mc_nvs_erase(nh, "k") == 0;

    hal::mock::MockI2c i2c;
    const int ih = mc_i2c_register(&i2c);
    uint8_t ibuf[4]{};
    uint32_t igot = 0u;
    bool i2c_ok = ih >= 0 && mc_i2c_write(ih, 0x50u, tx, 2u, 100u) == 0 &&
                  mc_i2c_read(ih, 0x50u, ibuf, sizeof(ibuf), 100u, &igot) == 0 &&
                  igot == 2u && ibuf[0] == 1u && ibuf[1] == 2u;

    TestFactory factory;
    const int fh = mc_net_register(&factory);
    const int sock = fh >= 0 ? mc_net_socket(fh, 0u) : -1;
    uint8_t ip[4] = {127, 0, 0, 1};
    bool net_ok = sock >= 0 && mc_net_connect(sock, ip, 80u, 100u) == 0;
    uint32_t sent = 0u, got = 0u;
    uint8_t rbuf[4]{};
    net_ok = net_ok && mc_net_send(sock, "GET", 3u, &sent) == 0 && sent == 3u;
    net_ok = net_ok && mc_net_recv(sock, rbuf, sizeof(rbuf), 100u, &got) == 0 &&
             got == 2u && rbuf[0] == 'A';
    mc_net_close(sock);

    TestUsbHal usb;
    const int uh = mc_usb_register(&usb);
    bool usb_ok = uh >= 0 && mc_usb_write(uh, 0x81u, "U", 1u) == 0 &&
                  usb.ep_ == 0x81u;
    uint8_t ubuf[4]{};
    uint32_t ugot = 0u;
    usb_ok = usb_ok && mc_usb_read(uh, 0x01u, ubuf, sizeof(ubuf), 100u, &ugot) == 0 &&
             ugot == 2u && ubuf[0] == 'U';

    const unsigned int v = mcframe_rust_selfcheck();
    std::printf("rust selfcheck=0x%08x available=%d\n", v,
                static_cast<int>(rust::available()));
    std::printf("ffi gpio=%d spi=%d nvs=%d i2c=%d net=%d usb=%d\n",
                gpio_ok, spi_ok, nvs_ok, i2c_ok, net_ok, usb_ok);
    return (v != 0u && rust::available() && gpio_ok && spi_ok && nvs_ok &&
            i2c_ok && net_ok && usb_ok) ? 0 : 1;
}
