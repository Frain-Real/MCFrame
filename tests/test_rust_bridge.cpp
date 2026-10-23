// tests/test_rust_bridge.cpp — host smoke-test C ABI Rust <-> C++.
#include "os/rust/rust_bridge.hpp"
#include "hal/mock/mock_hal.hpp"
#include <cstdio>

extern "C" unsigned int mcframe_rust_selfcheck(void);

int main() {
    embed::hal::mock::MockGpioPin pin;
    embed::hal::mock::MockSpi spi;
    (void)spi.configure(embed::hal::SpiConfig{});
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
    const unsigned int v = mcframe_rust_selfcheck();
    std::printf("rust selfcheck=0x%08x available=%d\n", v,
                static_cast<int>(embed::rust::available()));
    std::printf("ffi gpio=%d spi=%d\n", gpio_ok, spi_ok);
    return (v != 0u && embed::rust::available() && gpio_ok && spi_ok) ? 0 : 1;
}
