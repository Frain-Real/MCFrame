// tests/host/test_stm32f4_regs.cpp
#include "stm32f4_regs_fake.hpp"

#include <cstdio>

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
    (void)std::puts("=== F4/G4 register models ===");
    using namespace fake_stm32f4;

    check("RCC base", kRccBase == 0x40023800u);
    check("GPIOA base", kGpioABase == 0x40020000u);
    check("SPI1 base", kSpi1Base == 0x40013000u);
    check("TIM2 base", kTim2Base == 0x40000000u);

    rcc().AHB1ENR |= kRccGpioAEn;
    check("RCC enable", (rcc().AHB1ENR & kRccGpioAEn) != 0u);

    gpio_a().MODER = kGpioModeOutput;
    check("GPIO MODER", gpio_a().MODER == kGpioModeOutput);
    gpio_a().BSRR = 1u;
    check("GPIO BSRR", gpio_a().BSRR == 1u);

    spi1().CR1 = kSpiCr1Spe;
    spi1().DR = 0x5Au;
    check("SPI CR1/DR", spi1().CR1 == kSpiCr1Spe && spi1().DR == 0x5Au);

    tim2().ARR = 999u;
    tim2().CR1 = kTimCr1Cen;
    check("TIM ARR/CR1", tim2().ARR == 999u && tim2().CR1 == kTimCr1Cen);

    (void)std::printf("\n%d/%d проверок пройдено", g_total - g_failed, g_total);
    if (g_failed == 0) (void)std::printf(" — все тесты пройдены\n");
    else               (void)std::printf(" — ЕСТЬ ОШИБКИ\n");
    return g_failed == 0 ? 0 : 1;
}
