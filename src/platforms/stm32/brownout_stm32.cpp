// PVD для STM32F1.
#include "../../os/power/brownout.hpp"
#include "stm32f10x.h"

namespace embed::power {
namespace {
Brownout* g_pvd = nullptr;
constexpr uint32_t kPwrCrPvde = 1u << 4;
constexpr uint32_t kPwrCrPls  = 0x7u << 5;
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
uint32_t level_mv(uint32_t bits) noexcept { return 2200u + bits * 100u; }
} // namespace

Brownout& Brownout::instance() noexcept {
    static Brownout b;
    return b;
}

Status Brownout::init(uint32_t threshold_mv, Callback cb, void* ctx) noexcept {
    if (threshold_mv == 0u) threshold_mv = 2900u;
    cb_ = cb;
    ctx_ = ctx;
    threshold_mv_ = level_mv(level_bits(threshold_mv));
    g_pvd = this;
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR = (PWR->CR & ~kPwrCrPls) | (level_bits(threshold_mv) << 5) | kPwrCrPvde;
    EXTI->IMR |= 1u << 16;
    EXTI->FTSR |= 1u << 16;
    EXTI->RTSR |= 1u << 16;
    EXTI->PR = 1u << 16;
    NVIC_EnableIRQ(PVD_IRQn);
    enabled_ = true;
    return Status::Ok;
}

void Brownout::deinit() noexcept {
    PWR->CR &= ~kPwrCrPvde;
    EXTI->IMR &= ~(1u << 16);
    NVIC_DisableIRQ(PVD_IRQn);
    enabled_ = false;
    if (g_pvd == this) g_pvd = nullptr;
}

bool Brownout::below() const noexcept {
    return enabled_ && (PWR->CSR & PWR_CSR_PVDO) != 0u;
}

void Brownout::isr() noexcept {
    if (!enabled_) return;
    if (cb_ != nullptr) cb_(ctx_);
}

} // namespace embed::power

extern "C" void PVD_IRQHandler(void) {
    EXTI->PR = 1u << 16;
    embed::power::Brownout::instance().isr();
}
