// tests/host/test_stm32_periph.cpp
#include "stm32_periph.hpp"

#include <cstdio>

namespace embed::stm32 {
// В mock-сборке EXTI-реализации нет; для линковки vtable GpioPin.
Status exti_attach(GPIO_TypeDef*, uint8_t, hal::GpioEdge,
                   hal::GpioIrqCallback, void*) noexcept {
    return Status::NotSupported;
}
void exti_detach(uint8_t) noexcept {}
} // namespace embed::stm32

namespace {
int g_failed = 0;
int g_total = 0;
void check(const char* what, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    (void)std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}
} // namespace

int main() {
    (void)std::puts("=== STM32F1 GPIO/SPI/PWM register test ===");
    using namespace embed;
    using namespace embed::stm32;

    GpioPin led{GPIOA, 5u};
    hal::GpioConfig out{};
    out.mode = hal::GpioMode::Output;
    out.initial = true;
    check("GPIO configure", led.configure(out) == Status::Ok);
    led.set(true);
    check("GPIO set BSRR", (GPIOA->BSRR & (1u << 5)) != 0u);
    led.set(false);
    check("GPIO clear BRR", (GPIOA->BRR & (1u << 5)) != 0u);
    GPIOA->IDR = (1u << 5);
    led.toggle();
    check("GPIO toggle BRR", (GPIOA->BRR & (1u << 5)) != 0u);
    check("GPIO RCC", (RCC->APB2ENR & RCC_APB2ENR_IOPAEN) != 0u);

    GpioPin sck{GPIOA, 5u};
    GpioPin miso{GPIOA, 6u};
    GpioPin mosi{GPIOA, 7u};
    Spi spi{SPI1, 72000000u, sck, miso, mosi};
    hal::SpiConfig sc{};
    sc.hz = 1000000u;
    sc.mode = 0u;
    check("SPI configure", spi.configure(sc) == Status::Ok);
    check("SPI SPE", (SPI1->CR1 & SPI_CR1_SPE) != 0u);
    check("SPI MSTR", (SPI1->CR1 & SPI_CR1_MSTR) != 0u);
    check("SPI lock", spi.lock(10u) == Status::Ok);
    spi.unlock();

    GpioPin pwm_pin{GPIOA, 0u};
    Pwm pwm{TIM2, 1u, pwm_pin, 72000000u};
    hal::PwmConfig pc{};
    pc.frequency_hz = 1000u;
    pc.duty_percent = 25u;
    check("PWM configure", pwm.configure(pc) == Status::Ok);
    check("PWM PSC", TIM2->PSC == 71u);
    check("PWM ARR", TIM2->ARR == 999u);
    check("PWM duty", TIM2->CCR1 == 250u);
    check("PWM start", pwm.start() == Status::Ok && (TIM2->CR1 & TIM_CR1_CEN) != 0u);

    (void)std::printf("\n%d/%d проверок пройдено", g_total - g_failed, g_total);
    if (g_failed == 0) (void)std::printf(" — все тесты пройдены\n");
    else               (void)std::printf(" — ЕСТЬ ОШИБКИ\n");
    return g_failed == 0 ? 0 : 1;
}
