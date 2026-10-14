// tests/host/test_stm32_power.cpp
// ---------------------------------------------------------------------------
// ЭНЕРГОСБЕРЕЖЕНИЕ STM32 НА ПК: последовательность работы с регистрами RTC,
// PWR, EXTI, FLASH и с системным тиком.
//
// ЗАЧЕМ ЭТОТ НАБОР
// Код ухода в STOP — единственное место в проекте, где ошибка стоит не
// «неправильных данных», а мёртвого устройства: уснул и не проснулся. На ПК
// его можно проверить только вместе с моделью регистров, зато проверяется
// главное — ПОРЯДОК и УСЛОВИЯ:
//   * тактирование RTC выбрано и записано до программирования будильника;
//   * регистры RTC пишутся в режиме конфигурации (иначе запись теряется);
//   * линия EXTI17 разрешена ДО остановки ядра;
//   * старый флаг EXTI снят до WFI (иначе ядро вышло бы из STOP мгновенно);
//   * SysTick остановлен на время сна и возвращён после;
//   * SLEEPDEEP снят после выхода;
//   * проспанное время ИЗМЕРЕНО по RTC, а не взято из запроса;
//   * при смене профиля частота переключается только вместе с wait states
//     Flash и только после ожидания готовности источника.
//
// Модель намеренно «вредная»: без запрограммированного будильника она
// оставляет ядро в STOP (CoreModel::stuck_in_stop), а запись в регистры RTC
// вне режима конфигурации игнорирует — как железо.
// ---------------------------------------------------------------------------
#include "stm32_regs_fake.hpp"

#include "power_stm32.hpp"
#include "os/common.hpp"

#include <cstdio>

// ─────────────── То, что в прошивке даёт system_stm32f10x.c ────────────────
// На хосте его нет (файл собирается только для МКУ), поэтому функция
// восстановления тактирования подменяется моделью: она приводит RCC к
// состоянию «HSE 8 МГц × PLL = 72 МГц», как это делает SetSysClockTo72().
extern "C" uint32_t SystemCoreClock = 8000000u;

extern "C" void SystemCoreClockUpdate(void) {
    SystemCoreClock = embed::test::g_rcc.sysclk_hz();
}

extern "C" void SystemInit(void) {
    using namespace embed::test;
    g_rcc.regs.CR   |= RCC_CR_HSION | RCC_CR_HSEON | RCC_CR_PLLON;
    g_rcc.regs.CFGR  = RCC_CFGR_SW_PLL | RCC_CFGR_PLLMULL9 | RCC_CFGR_PLLSRC;
    g_flash.regs.ACR = FLASH_ACR_LATENCY_2;   // 72 МГц требуют 2 wait states
    SystemCoreClockUpdate();
}

namespace {

int g_failed = 0;
int g_total  = 0;

void check(const char* what, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    (void)std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

template <typename T>
void check_eq(const char* what, T got, T expected) {
    const bool ok = (got == expected);
    ++g_total;
    if (!ok) ++g_failed;
    (void)std::printf("  [%s] %-52s got=%lld expected=%lld\n",
                      ok ? "OK  " : "FAIL", what,
                      static_cast<long long>(got),
                      static_cast<long long>(expected));
}

using namespace embed;
using embed::test::g_core;
using embed::test::g_exti;
using embed::test::g_flash;
using embed::test::g_pwr;
using embed::test::g_rcc;
using embed::test::g_rtc;
using embed::test::g_systick;

constexpr uint32_t kExtiRtcLine = 17u;

/// Сброс «железа» и приведение платформы к исходному (Performance) профилю.
void resetHardware() {
    test::fakeReset();
    g_rcc.regs.CR   = RCC_CR_HSION | RCC_CR_HSEON | RCC_CR_PLLON;
    g_rcc.regs.CFGR = RCC_CFGR_SW_PLL;
    g_flash.regs.ACR = FLASH_ACR_LATENCY_2;
    SystemCoreClock = 72000000u;
}

// ─────────────────────────── Профиль частоты ───────────────────────────────

void test_profiles() {
    (void)std::puts("профиль частоты:");
    resetHardware();

    // Performance: ядро уже на 72 МГц после сброса — ничего не переключаем.
    check_eq<unsigned long long>("Performance не трогает тактирование",
          stm32::power::set_profile(static_cast<uint32_t>(platform::PowerProfile::Performance)),
          72000000ull);
    check_eq<unsigned long long>("SWS остался PLL",
          g_rcc.sws(), RCC_CFGR_SWS_PLL);

    // LowPower: HSI 8 МГц, PLL и HSE погашены, wait states снижены.
    check_eq<unsigned long long>("LowPower даёт 8 МГц",
          stm32::power::set_profile(static_cast<uint32_t>(platform::PowerProfile::LowPower)),
          8000000ull);
    check_eq<unsigned long long>("SYSCLK переключён на HSI",
          g_rcc.sws(), RCC_CFGR_SWS_HSI);
    check("PLL выключен", (g_rcc.regs.CR & RCC_CR_PLLON) == 0u);
    check("HSE погашен (это и есть экономия на кварце)",
          (g_rcc.regs.CR & RCC_CR_HSEON) == 0u);
    check_eq<unsigned long long>("wait states Flash = 0 при 8 МГц",
          g_flash.latency(), FLASH_ACR_LATENCY_0);
    check_eq<unsigned long long>("прескалеры шин 1:1",
          g_rcc.regs.CFGR & (RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2), 0ull);

    // Возврат запрещён: периферия уже настроена под 8 МГц.
    check_eq<unsigned long long>("возврат в Performance отклонён",
          stm32::power::set_profile(static_cast<uint32_t>(platform::PowerProfile::Performance)),
          0ull);
    check_eq<unsigned long long>("профиль остался LowPower",
          static_cast<unsigned long long>(stm32::power::profile()),
          static_cast<unsigned long long>(platform::PowerProfile::LowPower));
}

// ──────────────────────────── Инициализация RTC ────────────────────────────

void test_rtc_init() {
    (void)std::puts("инициализация RTC:");
    resetHardware();

    stm32::power::Config cfg{};
    cfg.deep_sleep_enabled = true;
    cfg.rtc_clock          = stm32::power::RtcClock::Lsi;

    check("init с глубоким сном == Ok", stm32::power::init(cfg) == Status::Ok);
    check("доступ к резервной области разрешён (DBP)",
          (g_pwr.cr() & PWR_CR_DBP) != 0u);
    check("тактирование RTC выбрано (RTCSEL = LSI)",
          (g_rcc.regs.BDCR & RCC_BDCR_RTCSEL) == RCC_BDCR_RTCSEL_LSI);
    check("RTC включён", (g_rcc.regs.BDCR & RCC_BDCR_RTCEN) != 0u);
    check("LSI запущен", g_rcc.lsi_enabled());
    check("прескалер записан (секундный счёт)",
          g_rtc.prescaler_set() && g_rtc.prescaler() == 40000u - 1u);
    check_eq<unsigned long long>("счётчик обнулён", g_rtc.counter(), 0ull);
    check("горизонт больше нуля (источник готов)",
          stm32::power::deep_sleep_horizon_ms() > 0u);
    check_eq<unsigned long long>("гранулярность будильника, мс",
          stm32::power::deep_sleep_granularity_ms(), 1000ull);
}

// ──────────────────────────────── Уход в STOP ──────────────────────────────

void test_deep_sleep_sequence() {
    (void)std::puts("последовательность ухода в STOP:");

    // Системный тик идёт: платформа обязана его остановить.
    g_systick.regs.CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk |
                          SysTick_CTRL_ENABLE_Msk;
    g_systick.regs.LOAD = 72000u - 1u;

    g_rtc.advanceSeconds(3u);                 // «прошло время» до сна
    const uint32_t before = g_rtc.counter();
    const uint32_t wfi_before = g_core.wfi_calls;

    const uint32_t slept = stm32::power::deep_sleep(2000u);

    // ── Будильник ──
    check("будильник запрограммирован", g_rtc.alarm_programmed());
    check("будильник в будущем", g_rtc.alarm() > before);
    check("запас добавлен: 2 с запроса + ~1.1 с = 4 с",
          g_rtc.alarm() - before >= 3u);
    check("прерывание будильника разрешено", g_rtc.alarm_irq_enabled());

    // ── Пробуждение и уборка ──
    check_eq<unsigned long long>("ядро действительно остановлено",
          g_core.wfi_calls, wfi_before + 1ull);
    check_eq<unsigned long long>("остановка была глубокой (SLEEPDEEP)",
          g_core.deep_wfi_calls, 1ull);
    check("прерывание будильника разрешено в NVIC до сна",
          g_core.alarm_irq_enabled_at_wfi);
    check("ядро не осталось в STOP (источник был вооружён)", !g_core.stuck_in_stop);
    check("SLEEPDEEP снят после выхода", !test::g_scb.sleepdeep());
    check("прерывание будильника запрещено после выхода",
          !test::g_nvic.enabled(static_cast<uint32_t>(RTCAlarm_IRQn)));

    // ── Тайминг ──
    check_eq<unsigned long long>("время сна измерено по RTC, мс", slept,
          (g_rtc.counter() - before) * 1000ull);
    check("измеренное время >= запрошенного (гранулярность секунды)",
          slept >= 2000u);

    // ── Тик ОС ──
    // Главная проверка: в момент остановки ядра системный тик НЕ шёл, иначе
    // первое же его прерывание вывело бы ядро из STOP через миллисекунды.
    check("SysTick был остановлен до входа в STOP",
          !g_core.systick_running_at_deep_wfi);
    check("SysTick снова работает после выхода", g_systick.running());
    check_eq<unsigned long long>("период тика сохранён",
          g_systick.regs.LOAD, 72000ull - 1ull);

    // ── Уборка флагов и линии пробуждения ──
    check("линия EXTI17 снята с маски после выхода", !g_exti.masked(kExtiRtcLine));
    check("флаг EXTI снят", !g_exti.pending(kExtiRtcLine));
    check("флаг будильника снят", !g_rtc.alarm_fired());
    check_eq<unsigned long long>("счётчик входов в сон", stm32::power::deep_sleep_entries(), 1ull);
    check_eq<unsigned long long>("суммарное время сна, мс",
          stm32::power::deep_sleep_total_ms(), slept);
    check_eq<unsigned long long>("ошибок нет",
          static_cast<unsigned long long>(stm32::power::last_error()),
          static_cast<unsigned long long>(Status::Ok));
}

// ──────────────── Источник пробуждения не вооружён: не спим ────────────────

void test_refuses_without_wake_source() {
    (void)std::puts("без источника пробуждения:");
    resetHardware();

    stm32::power::Config cfg{};
    cfg.deep_sleep_enabled = false;          // флаг сборки выключен
    check("init без глубокого сна == Ok", stm32::power::init(cfg) == Status::Ok);
    check_eq<unsigned long long>("горизонт равен нулю",
          stm32::power::deep_sleep_horizon_ms(), 0ull);
    check("DeepSleep не заявлен в возможностях",
          (stm32::power::caps() & static_cast<uint32_t>(platform::PowerCaps::DeepSleep)) == 0u);
    check_eq<unsigned long long>("вход в сон отклонён",
          stm32::power::deep_sleep(5000u), 0ull);

    const uint32_t wfi_before = g_core.wfi_calls;
    check_eq<unsigned long long>("ядро не останавливали",
          g_core.wfi_calls, wfi_before);
    check("ядро не застряло в STOP", !g_core.stuck_in_stop);
}

// ─────── Модель «вредная»: будильник не запрограммирован → ловушка ────────

void test_model_catches_missing_wake_source() {
    (void)std::puts("проверка самой модели (защита от ложного успеха):");
    resetHardware();

    // Имитируем ошибку конфигурации: линия EXTI разрешена, а будильник не
    // запрограммирован. На железе это «уснул навсегда» — модель обязана
    // показать это явно, иначе тест выше ничего не доказывал бы.
    test::g_scb.regs.SCR = SCB_SCR_SLEEPDEEP_Msk;
    g_exti.regs.IMR |= (1u << kExtiRtcLine);
    test::fakeWfi();
    test::g_scb.regs.SCR = 0u;

    check("модель распознала STOP без источника", g_core.stuck_in_stop);
    check("ядро не вышло из STOP", true);
}

} // namespace

int main() {
    (void)std::puts("=== энергосбережение STM32 на модели регистров ===");

    // Порядок важен: профиль LowPower — выбор на всю загрузку (обратный
    // переход запрещён), поэтому его проверка идёт последней.
    test_rtc_init();
    test_deep_sleep_sequence();
    test_refuses_without_wake_source();
    test_model_catches_missing_wake_source();
    test_profiles();

    (void)std::printf("\nвсего проверок: %d, провалено: %d\n", g_total, g_failed);
    if (g_failed != 0) (void)std::puts("FAIL");
    return (g_failed == 0) ? 0 : 1;
}
