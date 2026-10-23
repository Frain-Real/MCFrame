// platforms/stm32f4g4/rtc_m4.cpp
#include "rtc_m4.hpp"
#include "stm32m4_min.h"
#include "os/time/timer.hpp"

namespace embed::stm32f4g4::rtc {
namespace {

constexpr uint32_t kMagic = 0x4D43u;   // 'MC'
constexpr uint32_t kBdLseOn  = 1u << 0;
constexpr uint32_t kBdLseRdy = 1u << 1;
constexpr uint32_t kBdRtcSel = 3u << 8;
constexpr uint32_t kBdRtcEn  = 1u << 15;
constexpr uint32_t kCsLsiOn  = 1u << 0;
constexpr uint32_t kCsLsiRdy = 1u << 1;
constexpr uint32_t kPwrDbp   = 1u << 8;
constexpr uint32_t kIsrInit  = 1u << 7;
constexpr uint32_t kIsrInitf = 1u << 6;
constexpr uint32_t kIsrRsf   = 1u << 5;

uint8_t to_bcd(uint8_t v) noexcept { return static_cast<uint8_t>(((v / 10u) << 4) | (v % 10u)); }
uint8_t from_bcd(uint8_t v) noexcept { return static_cast<uint8_t>(((v >> 4) * 10u) + (v & 0x0Fu)); }

bool wait_bits(volatile uint32_t& reg, uint32_t mask, uint32_t want, uint32_t limit) noexcept {
    while ((reg & mask) != want) {
        if (--limit == 0u) return false;
    }
    return true;
}

void unlock_rtc() noexcept {
    RTC->WPR = 0xCAu;
    RTC->WPR = 0x53u;
}
void lock_rtc() noexcept { RTC->WPR = 0xFFu; }

Status start_clock() noexcept {
    MCFRAME_RCC_APB1ENR |= (1u << 28);        // PWREN
    PWR->CR |= kPwrDbp;

    uint32_t bdcr = MCFRAME_RCC_BDCR;
    if ((bdcr & kBdRtcEn) != 0u) return Status::Ok;

    uint32_t sel = bdcr & kBdRtcSel;
    if (sel == 0u) {
        // LSE: точнее и не зависит от температуры. Если кварца нет —
        // честно переключаемся на LSI (точность хуже, но время идёт).
        bdcr |= kBdLseOn;
        MCFRAME_RCC_BDCR = bdcr;
        if (!wait_bits(MCFRAME_RCC_BDCR, kBdLseRdy, kBdLseRdy, 4000000u)) {
            MCFRAME_RCC_CSR |= kCsLsiOn;
            if (!wait_bits(MCFRAME_RCC_CSR, kCsLsiRdy, kCsLsiRdy, 4000000u)) {
                return Status::HardwareError;
            }
            bdcr = (MCFRAME_RCC_BDCR & ~kBdRtcSel) | (2u << 8);
        } else {
            bdcr = (MCFRAME_RCC_BDCR & ~kBdRtcSel) | (1u << 8);
        }
    }
    bdcr |= kBdRtcEn;
    MCFRAME_RCC_BDCR = bdcr;

    unlock_rtc();
    RTC->ISR |= kIsrInit;
    if (!wait_bits(RTC->ISR, kIsrInitf, kIsrInitf, 100000u)) {
        lock_rtc();
        return Status::HardwareError;
    }
    // LSE 32768 Гц: async=128, sync=256 (PREDIV_A=127, PREDIV_S=255).
    RTC->PRER = (127u << 16) | 255u;
    RTC->ISR &= ~kIsrInit;
    if (!wait_bits(RTC->ISR, kIsrInitf, 0u, 100000u)) {
        lock_rtc();
        return Status::HardwareError;
    }
    if (!wait_bits(RTC->ISR, kIsrRsf, kIsrRsf, 100000u)) {
        lock_rtc();
        return Status::HardwareError;
    }
    lock_rtc();
    return Status::Ok;
}

DateTime read_datetime() noexcept {
    DateTime dt{};
    if ((RTC->ISR & kIsrRsf) == 0u) return dt;
    const uint32_t tr = RTC->TR;
    const uint32_t dr = RTC->DR;
    if ((tr & (1u << 6)) != 0u) return dt;   // 12-hour mode не поддерживаем
    dt.second = from_bcd(static_cast<uint8_t>(tr & 0x7Fu));
    dt.minute = from_bcd(static_cast<uint8_t>((tr >> 8) & 0x7Fu));
    dt.hour   = from_bcd(static_cast<uint8_t>((tr >> 16) & 0x3Fu));
    dt.day    = from_bcd(static_cast<uint8_t>(dr & 0x3Fu));
    dt.month  = from_bcd(static_cast<uint8_t>((dr >> 16) & 0x1Fu));
    dt.year   = static_cast<uint16_t>(2000u + from_bcd(static_cast<uint8_t>((dr >> 24) & 0xFFu)));
    return dt;
}

} // namespace

bool Backend::clocked() const noexcept {
    return (MCFRAME_RCC_BDCR & kBdRtcEn) != 0u;
}

Status Backend::init() noexcept {
    const Status st = start_clock();
    if (st != Status::Ok) return st;
    // Если время уже было записано — не трогаем календарь.
    if (RTC->BKP[0] == kMagic) return Status::Ok;
    return Status::TimeNotSynced;
}

Status Backend::set_datetime(uint64_t epoch_sec) noexcept {
    if (epoch_sec == 0u) return Status::InvalidArg;
    const DateTime dt = FromEpochSeconds(epoch_sec);
    if (dt.year < 2000u || dt.year > 2099u || !IsValid(dt)) return Status::InvalidArg;

    unlock_rtc();
    RTC->ISR |= kIsrInit;
    if (!wait_bits(RTC->ISR, kIsrInitf, kIsrInitf, 100000u)) {
        lock_rtc();
        return Status::HardwareError;
    }
    RTC->TR = (static_cast<uint32_t>(to_bcd(dt.hour)) << 16) |
              (static_cast<uint32_t>(to_bcd(dt.minute)) << 8) |
              static_cast<uint32_t>(to_bcd(dt.second));
    RTC->DR = (static_cast<uint32_t>(to_bcd(static_cast<uint8_t>(dt.year - 2000u))) << 24) |
              (static_cast<uint32_t>(to_bcd(dt.month)) << 16) |
              (1u << 8) |   // weekday: понедельник — не используется
              static_cast<uint32_t>(to_bcd(dt.day));
    RTC->ISR &= ~kIsrInit;
    if (!wait_bits(RTC->ISR, kIsrInitf, 0u, 100000u)) {
        lock_rtc();
        return Status::HardwareError;
    }
    (void)wait_bits(RTC->ISR, kIsrRsf, kIsrRsf, 100000u);
    RTC->BKP[0] = kMagic;
    lock_rtc();
    return Status::Ok;
}

Status Backend::read(uint64_t& epoch_sec) noexcept {
    const Status st = start_clock();
    if (st != Status::Ok) return st;
    if (RTC->BKP[0] != kMagic) return Status::TimeNotSynced;
    const DateTime dt = read_datetime();
    if (!IsValid(dt) || dt.year < 2000u) return Status::TimeNotSynced;
    epoch_sec = ToEpochSeconds(dt);
    if (epoch_sec == 0u) return Status::TimeNotSynced;
    return Status::Ok;
}

Status Backend::write(uint64_t epoch_sec) noexcept {
    const Status st = start_clock();
    if (st != Status::Ok) return st;
    return set_datetime(epoch_sec);
}

Backend& backend() noexcept {
    static Backend b;
    return b;
}

} // namespace embed::stm32f4g4::rtc
