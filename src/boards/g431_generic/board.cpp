#include "boards/g431_generic/board.hpp"
#include "os/nvs/nvs.hpp"
#include "platforms/stm32f4g4/rtc_m4.hpp"

#ifndef MCFRAME_NVS_SIZE
#define MCFRAME_NVS_SIZE 0u
#endif
#ifndef MCFRAME_NVS_OFFSET
#define MCFRAME_NVS_OFFSET 0u
#endif

namespace {

namespace d = embed::stm32::dma;

// NVS: 4 КБ в конце Flash у flat-образа; для slot-образа 0 (128 КБ заняты
// bootloader + двумя слотами + метаданными). Внешнее хранилище — через
// приложение и hal::INvs.
embed::nvs::HalNvs g_nvs;
#if MCFRAME_NVS_SIZE > 0
embed::ota::InternalFlashStorage g_nvs_flash{MCFRAME_NVS_OFFSET, MCFRAME_NVS_SIZE};
embed::nvs::Store                g_nvs_store;
#endif

embed::stm32::Uart1 g_uart1{board::kPins.pin(board::Signal::UartTx, 1u),
                            board::kPins.pin(board::Signal::UartRx, 1u),
                            board::kPins.alternate(board::Signal::UartTx, 1u)};
embed::stm32::Uart2 g_uart2{board::kPins.pin(board::Signal::UartTx, 2u),
                            board::kPins.pin(board::Signal::UartRx, 2u),
                            board::kPins.alternate(board::Signal::UartTx, 2u)};

embed::stm32::I2c1 g_i2c1{board::kPins.pin(board::Signal::I2cScl, 1u),
                          board::kPins.pin(board::Signal::I2cSda, 1u),
                          board::kPins.alternate(board::Signal::I2cScl, 1u)};
embed::stm32::I2c2 g_i2c2{board::kPins.pin(board::Signal::I2cScl, 2u),
                          board::kPins.pin(board::Signal::I2cSda, 2u),
                          board::kPins.alternate(board::Signal::I2cScl, 2u)};

embed::stm32::Adc1 g_adc1;

embed::stm32::Spi1 g_spi1{board::kPins.pin(board::Signal::SpiSck, 1u),
                          board::kPins.pin(board::Signal::SpiMiso, 1u),
                          board::kPins.pin(board::Signal::SpiMosi, 1u),
                          board::kPins.alternate(board::Signal::SpiSck, 1u),
                          d::route(d::Request::Spi1Tx),
                          d::route(d::Request::Spi1Rx)};
embed::stm32::Spi2 g_spi2{board::kPins.pin(board::Signal::SpiSck, 2u),
                          board::kPins.pin(board::Signal::SpiMiso, 2u),
                          board::kPins.pin(board::Signal::SpiMosi, 2u),
                          board::kPins.alternate(board::Signal::SpiSck, 2u),
                          d::route(d::Request::Spi2Tx),
                          d::route(d::Request::Spi2Rx)};

embed::stm32::GpioPin g_led{board::kPins.pin(board::Signal::Led, 0u)};
embed::stm32::GpioPin g_cs{board::kPins.pin(board::Signal::SpiCs, 1u)};

// W5500 на SPI1/CS. Инициализация сети — приложением: chip может быть
// не распаян, и board::init() не должен висеть на SPI.
embed::net::W5500FactoryT<4> g_net{g_spi1, g_cs};
embed::stm32::Pwm g_pwm{TIM2, 1u,
                        board::kPins.pin(board::Signal::Pwm, 2u, 1u),
                        board::kPins.alternate(board::Signal::Pwm, 2u, 1u),
                        SystemCoreClock};

} // namespace

namespace board {

Uart& uart1() noexcept { return g_uart1; }
Uart& uart2() noexcept { return g_uart2; }
I2c&  i2c1() noexcept  { return g_i2c1; }
I2c&  i2c2() noexcept  { return g_i2c2; }
Adc&  adc1() noexcept  { return g_adc1; }
Console& console() noexcept { return g_uart1; }
Gpio& led() noexcept { return g_led; }
Gpio& spi_cs() noexcept { return g_cs; }
Spi&  spi1() noexcept { return g_spi1; }
Spi&  spi2() noexcept { return g_spi2; }
Pwm&  pwm1() noexcept { return g_pwm; }
Nvs&  nvs() noexcept { return g_nvs; }
Net&  net() noexcept { return g_net; }

void init_peripherals() noexcept {
    embed::hal::GpioConfig out{};
    out.mode = embed::hal::GpioMode::Output;
    out.initial = false;
    (void)g_led.configure(out);

    embed::hal::GpioConfig cs{};
    cs.mode = embed::hal::GpioMode::Output;
    cs.initial = true;
    (void)g_cs.configure(cs);

    embed::hal::UartConfig uc{};
    uc.baud_rate = 115200u;
    (void)g_uart1.configure(uc);
    (void)g_uart1.open();

    // ADC-пины — в аналоговый режим. Драйвер ADC каналы не конфигурирует:
    // он не знает, на какой ноге какой канал (это знает карта платы).
    for (const auto& use : board::kPins.uses) {
        if (use.signal == board::Signal::Adc) {
            (void)embed::stm32::configure_pin(use.pin, embed::hal::GpioMode::Analog);
        }
    }
}

void init() noexcept {
    embed::rtc::WallClock::instance().attach(&embed::stm32f4g4::rtc::backend());
    init_peripherals();
#if MCFRAME_NVS_SIZE > 0
    if (g_nvs_store.init({&g_nvs_flash, MCFRAME_NVS_SIZE / 2u}) == embed::Status::Ok) {
        g_nvs.bind(&g_nvs_store);
    }
#endif
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
