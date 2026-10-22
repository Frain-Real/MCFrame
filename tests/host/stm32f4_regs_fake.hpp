// tests/host/stm32f4_regs_fake.hpp
// ---------------------------------------------------------------------------
// Phase 2: минимальные host-модели регистров F4/G4 для будущих драйверов
// (GPIO/SPI/TIM/RCC). Пока проверяется только раскладка/доступность полей;
// драйверы Phase 3 будут подменять MMIO этими объектами.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace fake_stm32f4 {

constexpr uintptr_t kRccBase  = 0x40023800u;
constexpr uintptr_t kGpioABase = 0x40020000u;
constexpr uintptr_t kSpi1Base = 0x40013000u;
constexpr uintptr_t kTim2Base = 0x40000000u;

struct Rcc {
    volatile uint32_t CR;
    volatile uint32_t PLLCFGR;
    volatile uint32_t CFGR;
    volatile uint32_t CIR;
    volatile uint32_t AHB1RSTR;
    volatile uint32_t AHB2RSTR;
    volatile uint32_t AHB3RSTR;
    volatile uint32_t RESERVED0;
    volatile uint32_t APB1RSTR;
    volatile uint32_t APB2RSTR;
    volatile uint32_t RESERVED1[2];
    volatile uint32_t AHB1ENR;
    volatile uint32_t AHB2ENR;
    volatile uint32_t AHB3ENR;
};

struct Gpio {
    volatile uint32_t MODER;
    volatile uint32_t OTYPER;
    volatile uint32_t OSPEEDR;
    volatile uint32_t PUPDR;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t LCKR;
    volatile uint32_t AFR[2];
};

struct Spi {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t CRCPR;
    volatile uint32_t RXCRCR;
    volatile uint32_t TXCRCR;
    volatile uint32_t I2SCFGR;
    volatile uint32_t I2SPR;
};

struct Tim {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SMCR;
    volatile uint32_t DIER;
    volatile uint32_t SR;
    volatile uint32_t EGR;
    volatile uint32_t CCMR1;
    volatile uint32_t CCMR2;
    volatile uint32_t CCER;
    volatile uint32_t CNT;
    volatile uint32_t PSC;
    volatile uint32_t ARR;
    volatile uint32_t RCR;
    volatile uint32_t CCR[4];
};

inline Rcc& rcc() noexcept { static Rcc r{}; return r; }
inline Gpio& gpio_a() noexcept { static Gpio g{}; return g; }
inline Spi& spi1() noexcept { static Spi s{}; return s; }
inline Tim& tim2() noexcept { static Tim t{}; return t; }

// Биты, которые будут использовать драйверы Phase 3.
constexpr uint32_t kRccGpioAEn = 1u << 0;
constexpr uint32_t kGpioModeOutput = 1u;
constexpr uint32_t kSpiCr1Spe = 1u << 6;
constexpr uint32_t kTimCr1Cen = 1u << 0;

} // namespace fake_stm32f4
