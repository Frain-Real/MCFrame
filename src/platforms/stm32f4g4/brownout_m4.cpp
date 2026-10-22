// PVD для STM32F4/G4 (порог и EXTI16; G4 использует тот же PWR_CR/PWR_CSR).
#include "../../os/power/brownout.hpp"
#include "stm32m4_min.h"

namespace embed::power {
namespace {
Brownout* g_pvd = nullptr;
constexpr uint32_t kPvde = 1u << 4;
constexpr uint32_t kPls  = 0x7u << 5;
uint32_t level_bits(uint32_t mv) noexcept {
    if (mv <= 2250u) return 0u;
    if (mv <= 2350u) return 1u;
    if (mv <= 2450u) return 2u;
    if (mv <= 2550u) return 3u;
    if (mv <= 2650u) return 4u;
    if (mv <= 2750u) return 5u;
    if (mv <= 2850u) return 6u;
    return 7u;
}
uint32_t level_mv(uint32_t b) noexcept { return 2200u + b * 100u; }
} // namespace

Brownout& Brownout::instance() noexcept { static Brownout b; return b; }

Status Brownout::init(uint32_t threshold_mv, Callback cb, void* ctx) noexcept {
    if (threshold_mv == 0u) threshold_mv = 2900u;
    cb_ = cb; ctx_ = ctx; g_pvd = this;
    threshold_mv_ = level_mv(level_bits(threshold_mv));
    RCC->APB1ENR |= 1u << 28;   // PWREN
    PWR->CR = (PWR->CR & ~kPls) | (level_bits(threshold_mv) << 5) | kPvde;
    EXTI->IMR |= 1u << 16;
    EXTI->FTSR |= 1u << 16;
    EXTI->RTSR |= 1u << 16;
    EXTI->PR = 1u << 16;
    NVIC_EnableIRQ(PVD_IRQn);
    enabled_ = true;
    return Status::Ok;
}
void Brownout::deinit() noexcept {
    PWR->CR &= ~kPvde;
    EXTI->IMR &= ~(1u << 16);
    NVIC_DisableIRQ(PVD_IRQn);
    enabled_ = false;
    if (g_pvd == this) g_pvd = nullptr;
}
bool Brownout::below() const noexcept { return enabled_ && (PWR->CSR & (1u << 2)) != 0u; }
void Brownout::isr() noexcept { if (enabled_ && cb_ != nullptr) cb_(ctx_); }
} // namespace embed::power

extern "C" void PVD_IRQHandler(void) {
    EXTI->PR = 1u << 16;
    embed::power::Brownout::instance().isr();
}
