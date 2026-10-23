// platforms/stm32f4g4/power_m4.cpp
// F4/G4: STOP + RTC wakeup и IWDG. Подключается ТОЛЬКО приложением, чтобы
// 16-КБ загрузчик не тянул лишний код.
#include "../../os/iplatform.hpp"
#include "../../arch/armv7m/arch.hpp"
#include "stm32m4_min.h"
#include "rtc_m4.hpp"

namespace embed::platform {

// ── Энергосбережение F4/G4: STOP + RTC wakeup ────────────────────────────────
uint32_t power_caps() noexcept {
    return static_cast<uint32_t>(PowerCaps::Sleep) |
           static_cast<uint32_t>(PowerCaps::DeepSleep);
}
uint32_t power_sysclk() noexcept { return SystemCoreClock; }
uint32_t power_profile(uint32_t profile) noexcept { return profile; }
uint32_t power_horizon() noexcept {
    // RTC wakeup timer работает в STOP; гарантируем не меньше 1 часа.
    return (MCFRAME_RCC_BDCR & (1u << 15)) != 0u ? 3600000u : 0u;
}
uint32_t power_granularity() noexcept { return 1000u; }

namespace {
bool rtc_wait(uint32_t mask, uint32_t want, uint32_t limit) noexcept {
    while ((RTC->ISR & mask) != want) {
        if (--limit == 0u) return false;
    }
    return true;
}
uint32_t ceil_seconds(uint32_t ms) noexcept {
    uint32_t s = ms / 1000u;
    if ((ms % 1000u) != 0u || s == 0u) ++s;
    return s;
}
}

uint32_t power_deep_sleep(uint32_t max_ms) noexcept {
    if (max_ms == 0u) max_ms = 1000u;
    const Status rst = stm32f4g4::rtc::backend().init();
    if (rst == Status::HardwareError) return 0u;

    uint32_t secs = ceil_seconds(max_ms);
    if (secs > 0xFFFFu) secs = 0xFFFFu;

    MCFRAME_RCC_APB1ENR |= (1u << 28);
    PWR->CR |= PWR_CR_DBP;

    // RTC wakeup timer: WUTE=0 -> WUTWF -> WUTR -> WUTE/WUTIE.
    RTC->WPR = 0xCAu; RTC->WPR = 0x53u;
    RTC->CR &= ~RTC_CR_WUTE;
    if (!rtc_wait(RTC_ISR_WUTWF, RTC_ISR_WUTWF, 100000u)) { RTC->WPR = 0xFFu; return 0u; }
    RTC->ISR &= ~RTC_ISR_WUTF;
    RTC->WUTR = secs - 1u;
    RTC->CR |= RTC_CR_WUTE | RTC_CR_WUTIE;
    RTC->WPR = 0xFFu;

    // STOP: LPDS=1, PDDS=0; SLEEPDEEP=1.
    PWR->CR = (PWR->CR | PWR_CR_LPDS) & ~PWR_CR_PDDS;
    volatile uint32_t* scr = reinterpret_cast<volatile uint32_t*>(0xE000ED10u);
    *scr |= (1u << 2);
    __DSB();
    __WFI();
    *scr &= ~(1u << 2);

    // После STOP ядро снова на HSI: восстановить PLL/часы.
    SystemInit();

    RTC->WPR = 0xCAu; RTC->WPR = 0x53u;
    RTC->CR &= ~(RTC_CR_WUTE | RTC_CR_WUTIE);
    RTC->ISR &= ~RTC_ISR_WUTF;
    RTC->WPR = 0xFFu;
    return max_ms;
}
PowerOps kPowerOps{&power_caps, &power_sysclk, &power_profile,
                         &power_horizon, &power_granularity, &power_deep_sleep};

// ── IWDG F4/G4 ───────────────────────────────────────────────────────────────
constexpr uint32_t kIwdgLsiHz = 32000u;
uint32_t wd_caps() noexcept {
    return static_cast<uint32_t>(WatchdogCaps::Hardware) |
           static_cast<uint32_t>(WatchdogCaps::ResetCause) |
           static_cast<uint32_t>(WatchdogCaps::BackupNote);
}
uint32_t wd_closest(uint32_t requested_ms) noexcept {
    if (requested_ms == 0u) return 0u;
    static constexpr uint32_t divs[4] = {4u, 8u, 32u, 128u};
    for (uint32_t d = 0u; d < 4u; ++d) {
        const uint64_t denom = 1000u * divs[d];
        uint64_t ticks = (static_cast<uint64_t>(requested_ms) * kIwdgLsiHz + denom - 1u) / denom;
        if (ticks > 4096u) ticks = 4096u;
        if (ticks > 0u && ticks <= 4096u) {
            return static_cast<uint32_t>((ticks * divs[d] * 1000u) / kIwdgLsiHz);
        }
    }
    return static_cast<uint32_t>((4095ull * 128u * 1000u) / kIwdgLsiHz);
}
bool wd_start(uint32_t timeout_ms) noexcept {
    if (timeout_ms == 0u) return false;
    static constexpr uint32_t divs[4] = {4u, 8u, 32u, 128u};
    for (uint32_t d = 0u; d < 4u; ++d) {
        const uint64_t ticks = (static_cast<uint64_t>(timeout_ms) * kIwdgLsiHz) /
                               (1000u * divs[d]);
        if (ticks >= 1u && ticks <= 4096u) {
            IWDG->KR = 0x5555u;
            // PVU/RVU: запись PR/RLR применяется не мгновенно.
            for (uint32_t g = 100000u; (IWDG->SR & 1u) != 0u && g != 0u; --g) { }
            IWDG->PR = d;
            for (uint32_t g = 100000u; (IWDG->SR & 2u) != 0u && g != 0u; --g) { }
            IWDG->RLR = static_cast<uint32_t>(ticks) - 1u;
            for (uint32_t g = 100000u; (IWDG->SR & 2u) != 0u && g != 0u; --g) { }
            IWDG->KR = 0xAAAAu;
            IWDG->KR = 0xCCCCu;
            return true;
        }
    }
    return false;
}
void wd_kick() noexcept { IWDG->KR = 0xAAAAu; }
bool wd_stop() noexcept { return false; }
bool wd_note_write(uint32_t index, uint16_t value) noexcept {
    if (index >= 20u) return false;
    MCFRAME_RCC_APB1ENR |= (1u << 28);
    PWR->CR |= PWR_CR_DBP;
    RTC->BKP[index] = value;
    return true;
}
uint16_t wd_note_read(uint32_t index) noexcept {
    if (index >= 20u) return 0u;
    MCFRAME_RCC_APB1ENR |= (1u << 28);
    PWR->CR |= PWR_CR_DBP;
    return static_cast<uint16_t>(RTC->BKP[index] & 0xFFFFu);
}
uint32_t wd_reset_cause() noexcept {
    const uint32_t csr = MCFRAME_RCC_CSR;
    // RMVF очищает все флаги причины сброса.
    MCFRAME_RCC_CSR |= (1u << 24);
    uint32_t out = static_cast<uint32_t>(ResetCause::Unknown);
    if ((csr & RCC_CSR_IWDGRSTF) != 0u) out |= static_cast<uint32_t>(ResetCause::IndependentWatchdog);
    if ((csr & RCC_CSR_SFTRSTF) != 0u) out |= static_cast<uint32_t>(ResetCause::Software);
    if ((csr & RCC_CSR_PORRSTF) != 0u) out |= static_cast<uint32_t>(ResetCause::PowerOn);
    if ((csr & RCC_CSR_PINRSTF) != 0u) out |= static_cast<uint32_t>(ResetCause::Pin);
    if ((csr & RCC_CSR_LPWRRSTF) != 0u) out |= static_cast<uint32_t>(ResetCause::LowPower);
    return out;
}
WatchdogOps kWatchdogOps{&wd_caps, &wd_closest, &wd_start, &wd_kick,
                               &wd_stop, &wd_note_write, &wd_note_read,
                               &wd_reset_cause};


} // namespace embed::platform
