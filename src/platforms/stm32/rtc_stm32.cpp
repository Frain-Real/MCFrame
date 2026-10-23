// platforms/stm32/rtc_stm32.cpp
#include "rtc_stm32.hpp"
#include "stm32_regs.hpp"

namespace embed::stm32::rtc {
namespace {

constexpr uint16_t kMagic = 0x4D43u;   // 'MC'

void enable_backup() noexcept {
    RCC->APB1ENR |= RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
    PWR->CR      |= PWR_CR_DBP;
}

bool rtc_clocked() noexcept {
    return (RCC->BDCR & RCC_BDCR_RTCEN) != 0u;
}

uint32_t counter() noexcept {
    const uint32_t h1 = RTC->CNTH;
    const uint32_t l  = RTC->CNTL;
    const uint32_t h2 = RTC->CNTH;
    if (h1 != h2) return (h2 << 16) | RTC->CNTL;
    return (h1 << 16) | l;
}

bool set_counter(uint32_t value) noexcept {
    // Последовательность RM0008: RTOFF -> CNF -> RTOFF -> запись -> снять CNF.
    uint32_t guard = 200000u;
    while ((RTC->CRL & RTC_CRL_RTOFF) == 0u && --guard != 0u) {}
    if (guard == 0u) return false;
    RTC->CRL |= RTC_CRL_CNF;
    guard = 200000u;
    while ((RTC->CRL & RTC_CRL_RTOFF) == 0u && --guard != 0u) {}
    if (guard == 0u) return false;
    RTC->CNTH = value >> 16;
    RTC->CNTL = value & 0xFFFFu;
    RTC->CRL &= ~RTC_CRL_CNF;
    guard = 200000u;
    while ((RTC->CRL & RTC_CRL_RTOFF) == 0u && --guard != 0u) {}
    return guard != 0u;
}

} // namespace

Status Backend::read(uint64_t& epoch_sec) noexcept {
    enable_backup();
    if (!rtc_clocked()) return Status::TimeNotSynced;
    if ((BKP->DR1 & 0xFFFFu) != kMagic) return Status::TimeNotSynced;
    epoch_sec = static_cast<uint64_t>(counter());
    return Status::Ok;
}

Status Backend::write(uint64_t epoch_sec) noexcept {
    enable_backup();
    if (!rtc_clocked()) return Status::NotSupported;
    if (epoch_sec == 0u || epoch_sec > 0xFFFFFFFFull) return Status::InvalidArg;
    if (!set_counter(static_cast<uint32_t>(epoch_sec))) return Status::HardwareError;
    BKP->DR1 = kMagic;
    return Status::Ok;
}

Backend& backend() noexcept {
    static Backend b;
    return b;
}

} // namespace embed::stm32::rtc
