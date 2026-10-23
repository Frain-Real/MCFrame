// tests/host/test_rtc_stm32.cpp
#include "platforms/stm32/rtc_stm32.hpp"
#include "stm32_regs_fake.hpp"

#include <cstdio>

using namespace embed;

namespace {
int total = 0, failed = 0;
void check(const char* what, bool ok) {
    ++total; if (!ok) ++failed;
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}
}

int main() {
    std::puts("=== STM32F1 RTC backend ===");

    // RTC выключен — backend обязан честно сказать TimeNotSynced.
    uint64_t epoch = 0u;
    check("RTC off read", stm32::rtc::backend().read(epoch) == Status::TimeNotSynced);

    // Включаем RTC и разрешаем backup-доступ.
    RCC->BDCR |= RCC_BDCR_RTCEN;
    PWR->CR |= PWR_CR_DBP;
    test::g_rtc.advanceSeconds(0x12345678u);
    BKP->DR1 = 0x4D43u;
    check("read magic", stm32::rtc::backend().read(epoch) == Status::Ok &&
                        epoch == 0x12345678ull);

    BKP->DR1 = 0u;
    check("no magic -> TimeNotSynced",
          stm32::rtc::backend().read(epoch) == Status::TimeNotSynced);

    check("write", stm32::rtc::backend().write(1777777777ull) == Status::Ok);
    check("read back", stm32::rtc::backend().read(epoch) == Status::Ok &&
                       epoch == 1777777777ull);
    check("magic written", BKP->DR1 == 0x4D43u);

    check("write 0 rejected", stm32::rtc::backend().write(0u) == Status::InvalidArg);

    std::printf("%d/%d проверок пройдено%s\n", total - failed, total,
                failed == 0 ? " — все тесты пройдены" : " — ЕСТЬ ОШИБКИ");
    return failed == 0 ? 0 : 1;
}
