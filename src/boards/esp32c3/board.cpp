#include "boards/esp32c3/board.hpp"
#include "boards/esp32c3/hal_esp32c3.hpp"
#include "boards/esp32c3/wifi_esp.hpp"
#include "boards/esp32c3/ble_esp.hpp"

namespace {

embed::esp32c3::Uart    g_console{UART_NUM_0};
embed::esp32c3::GpioPin g_led{static_cast<gpio_num_t>(BOARD_LED_GPIO)};
embed::esp32c3::GpioPin g_cs{static_cast<gpio_num_t>(BOARD_SPI_CS_GPIO)};
embed::esp32c3::Spi     g_spi{static_cast<gpio_num_t>(BOARD_SPI_MOSI_GPIO),
                              static_cast<gpio_num_t>(BOARD_SPI_MISO_GPIO),
                              static_cast<gpio_num_t>(BOARD_SPI_SCLK_GPIO)};
embed::esp32c3::Pwm     g_pwm{static_cast<gpio_num_t>(BOARD_PWM_GPIO)};
embed::esp32c3::Nvs     g_nvs;
embed::esp32c3::WifiEsp g_wifi;
embed::esp32c3::BleEsp  g_ble;

} // namespace

namespace board {

Uart& uart1() noexcept { return g_console; }
Console& console() noexcept { return g_console; }
Gpio& led() noexcept { return g_led; }
Gpio& spi_cs() noexcept { return g_cs; }
Spi& spi1() noexcept { return g_spi; }
Pwm& pwm1() noexcept { return g_pwm; }
Nvs& nvs() noexcept { return g_nvs; }
Network& wifi() noexcept { return g_wifi; }
Ble& ble() noexcept { return g_ble; }

void init() noexcept {
    embed::hal::GpioConfig out{};
    out.mode = embed::hal::GpioMode::Output;
    out.initial = false;
    (void)g_led.configure(out);
    embed::hal::GpioConfig cs{};
    cs.mode = embed::hal::GpioMode::Output;
    cs.initial = true;
    (void)g_cs.configure(cs);
    (void)g_console.configure(embed::hal::UartConfig{});
    (void)g_console.open();
    (void)g_nvs.init();
}

} // namespace board
