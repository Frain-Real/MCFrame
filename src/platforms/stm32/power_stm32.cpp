// platforms/stm32/power_stm32.cpp
// ---------------------------------------------------------------------------
// Энергосбережение STM32F103: Sleep (WFI), DeepSleep (STOP + RTC) и статический
// профиль частоты. Регистры — ТОЛЬКО через stm32_regs.hpp, поэтому этот файл
// собирается и в прошивку, и в хостовый набор на модели регистров.
//
// Глубокий сон компилируется по флагу MCFRAME_STM32_DEEP_SLEEP (по умолчанию
// выключен): пока источник пробуждения не проверен на конкретной плате,
// «уснул и не проснулся» — это не теоретический риск. Без флага платформа
// честно сообщает, что DeepSleep не поддерживает, и супервизор энергосбережения
// обходится Sleep/WFI.
// ---------------------------------------------------------------------------
#include "power_stm32.hpp"

#include "system_stm32f10x.h"   // SystemInit / SystemCoreClock / SystemCoreClockUpdate

namespace embed::stm32::power {

namespace {

// ───────────────────────────── Внутреннее состояние ─────────────────────────
Config   g_cfg{};
// g_inited/g_rtc_ready/g_rtc_prescaler читаются только кодом глубокого сна:
// без флага сборки MCFRAME_STM32_DEEP_SLEEP их использование исчезает, и
// [[maybe_unused]] честно говорит об этом компилятору.
[[maybe_unused]] bool     g_inited      = false;
[[maybe_unused]] bool     g_rtc_ready   = false;
[[maybe_unused]] uint32_t g_rtc_prescaler = 0u;

platform::PowerProfile g_profile = platform::PowerProfile::Performance;
uint32_t g_sysclk_hz = 0u;

Status   g_last_error = Status::Ok;
uint32_t g_entries    = 0u;
uint32_t g_total_ms   = 0u;
uint32_t g_failures   = 0u;

// ──────────────────────────────── Константы ────────────────────────────────
constexpr uint32_t kExtiRtcAlarmLine = 17u;         ///< EXTI17 — будильник RTC
constexpr uint32_t kLseHz        = 32768u;          ///< внешний кварц 32.768 кГц
constexpr uint32_t kLsiHzNominal = 40000u;          ///< LSI: номинал ~40 кГц
constexpr uint32_t kHsiHz        = 8000000u;        ///< HSI: 8 МГц
constexpr uint32_t kMaxDeepSleepMs = 3600u * 1000u; ///< верхняя граница одного сна

// Бюджеты ожидания готовности железа.
//
// Ждать «вечно» в супервизоре нельзя: если источник тактирования RTC не
// запустился, приложение обязано получить ошибку, а не встать навсегда.
// Счёт итераций переведён в циклы по наихудшему случаю (проверка бита —
// примерно 5 тактов), при 72 МГц:
//   * HSI/PLL/запись RTC — десятки микросекунд, берём с большим запасом;
//   * LSI запускается максимум за 85 мкс (DS5319, Tab.25) — 0.1 c хватает;
//   * LSE (кварц 32.768 кГц) запускается ТИПОВО 1.5–2.5 с (DS5319, Tab.23),
//     поэтому бюджет отдельный и большой: первый init с LSE может занять
//     секунды, и это нормальное поведение кварца, а не зависание.
constexpr uint32_t kSpinLimit     = 0x000FFFFFu;   ///< ≈ 0.05 с
constexpr uint32_t kLsiSpinLimit  = 0x00400000u;   ///< ≈ 0.15 с
constexpr uint32_t kLseSpinLimit  = 0x0A000000u;   ///< ≈ 7 с

/// Ожидание условия с ограничением. true — дождались.
template <typename Pred>
bool spin_until(Pred ready, uint32_t limit = kSpinLimit) noexcept {
    for (uint32_t i = 0u; i < limit; ++i) {
        if (ready()) return true;
    }
    return false;
}

// ─────────────────────────── Профили частоты ───────────────────────────────

/// Минимальное число wait states Flash для частоты (RM0008, таблица LATENCY):
/// 0–24 МГц — 0 WS, 24–48 МГц — 1 WS, 48–72 МГц — 2 WS.
constexpr uint32_t flash_latency_for(uint32_t hz) noexcept {
    if (hz <= 24000000u) return FLASH_ACR_LATENCY_0;
    if (hz <= 48000000u) return FLASH_ACR_LATENCY_1;
    return FLASH_ACR_LATENCY_2;
}

void set_flash_latency(uint32_t latency) noexcept {
    // LATENCY меняется ВМЕСТЕ с частотой и в правильном порядке: повышая
    // частоту — сначала wait states, понижая — сначала частоту. Здесь порядок
    // соблюдён вызывающими (см. apply_profile).
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | latency;
}

/// Профиль Performance: 72 МГц от HSE через PLL.
///
/// Реализуется тем же кодом, что и запуск после сброса (SystemInit): после
/// STOP ядро работает от HSI, и «вернуть 72 МГц» — это ровно та же
/// последовательность (включить HSE, дождаться HSERDY, поднять PLL,
/// дождаться PLLRDY, переключить SW, выставить latency).
void apply_performance() noexcept {
    SystemInit();
    SystemCoreClockUpdate();
    g_sysclk_hz = SystemCoreClock;
    g_profile   = platform::PowerProfile::Performance;
}

/// Профиль LowPower: 8 МГц от HSI, PLL выключен.
///
/// Что это даёт и чего не даёт: ток падает примерно втрое-вчетверо (снимается
/// PLL и HSE), но энергия на инструкцию НЕ падает — напряжение ядра на
/// STM32F1 не регулируется. Поэтому профиль имеет смысл только там, где
/// система обязана работать непрерывно; для импульсной нагрузки выгоднее
/// Performance + глубокий сон (см. README).
///
/// Периферия при этом остаётся настроенной на СТАРУЮ частоту (делители baud
/// USART, CCR у I2C, прескалер АЦП зависят от PCLK), поэтому профиль
/// применяется только до инициализации периферии — это требование контракта
/// platform::PowerOps::set_profile, а не пожелание.
bool apply_low_power() noexcept {
    // 1. Включить HSI и дождаться готовности.
    RCC->CR |= RCC_CR_HSION;
    if (!spin_until([] { return (RCC->CR & RCC_CR_HSIRDY) != 0u; })) {
        return false;
    }

    // 2. Переключить SYSCLK на HSI (PLL ещё работает как источник — его
    //    нельзя выключать раньше, чем переключимся).
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_HSI;
    if (!spin_until([] { return (RCC->CFGR & RCC_CFGR_SWS) == RCC_CFGR_SWS_HSI; })) {
        return false;
    }

    // 3. Теперь PLL можно выключить, а HSE — погасить (это и есть экономия).
    RCC->CR &= ~RCC_CR_PLLON;
    RCC->CR &= ~RCC_CR_HSEON;

    // 4. Понижаем частоту — можно уменьшить wait states Flash.
    set_flash_latency(flash_latency_for(kHsiHz));

    // 5. Шины 1:1: HCLK = PCLK1 = PCLK2 = 8 МГц.
    //    ADCCLK = PCLK2 / prescaler, предел 14 МГц — при 8 МГц любезен любой
    //    делитель, поэтому прескалер АЦП не трогаем (его настроит драйвер).
    RCC->CFGR &= ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2);

    SystemCoreClockUpdate();
    g_sysclk_hz = SystemCoreClock;
    g_profile   = platform::PowerProfile::LowPower;
    return true;
}

// ─────────────────────────────────── RTC ───────────────────────────────────
#if MCFRAME_STM32_DEEP_SLEEP

/// Доступ к регистрам резервной области: без DBP запись в RTC игнорируется.
void enable_backup_access() noexcept {
    RCC->APB1ENR |= RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
    PWR->CR      |= PWR_CR_DBP;
}

/// Источник тактирования RTC. RTCSEL записывается один раз до сброса резервной
/// области, поэтому «переключить» источник на ходу нельзя: сообщаем об ошибке,
/// а не сбрасываем домен (сброс убил бы и счётчик времени).
bool select_rtc_clock(RtcClock clk) noexcept {
    const uint32_t wanted = (clk == RtcClock::Lse) ? RCC_BDCR_RTCSEL_LSE
                                                   : RCC_BDCR_RTCSEL_LSI;

    if ((RCC->BDCR & RCC_BDCR_RTCSEL) == wanted) return true;   // уже выбран
    if ((RCC->BDCR & RCC_BDCR_RTCSEL) != 0u) return false;      // выбран другой

    if (clk == RtcClock::Lse) {
        RCC->BDCR |= RCC_BDCR_LSEON;
        // Кварц 32.768 кГц запускается типово 1.5–2.5 с: здесь бюджет
        // сознательно большой, иначе исправная плата получала бы отказ.
        if (!spin_until([] { return (RCC->BDCR & RCC_BDCR_LSERDY) != 0u; },
                        kLseSpinLimit)) {
            return false;
        }
        RCC->BDCR = (RCC->BDCR & ~RCC_BDCR_RTCSEL) | RCC_BDCR_RTCSEL_LSE;
    } else {
        RCC->CSR |= RCC_CSR_LSION;
        if (!spin_until([] { return (RCC->CSR & RCC_CSR_LSIRDY) != 0u; },
                        kLsiSpinLimit)) {
            return false;
        }
        RCC->BDCR = (RCC->BDCR & ~RCC_BDCR_RTCSEL) | RCC_BDCR_RTCSEL_LSI;
    }
    RCC->BDCR |= RCC_BDCR_RTCEN;
    return true;
}

/// Вход в режим конфигурации RTC. Последовательность из RM0008: дождаться
/// RTOFF (предыдущая запись выполнена), выставить CNF, снова дождаться RTOFF.
bool rtc_config_begin() noexcept {
    if (!spin_until([] { return (RTC->CRL & RTC_CRL_RTOFF) != 0u; })) return false;
    RTC->CRL |= RTC_CRL_CNF;
    if (!spin_until([] { return (RTC->CRL & RTC_CRL_RTOFF) != 0u; })) return false;
    return true;
}

/// Выход из режима конфигурации: снять CNF и дождаться применения записи.
bool rtc_config_end() noexcept {
    RTC->CRL &= ~RTC_CRL_CNF;
    return spin_until([] { return (RTC->CRL & RTC_CRL_RTOFF) != 0u; });
}

uint32_t rtc_counter() noexcept {
    // Два чтения подряд: CNTH/CNTL читаются не одновременно, поэтому пара
    // значений может «разъехаться» на секунду. Если старшее слово изменилось —
    // перечитываем младшее.
    const uint32_t h1 = RTC->CNTH;
    const uint32_t l  = RTC->CNTL;
    const uint32_t h2 = RTC->CNTH;
    if (h1 != h2) return (h2 << 16) | RTC->CNTL;
    return (h1 << 16) | l;
}

bool rtc_write_counter(uint32_t value) noexcept {
    if (!rtc_config_begin()) return false;
    RTC->CNTH = value >> 16;
    RTC->CNTL = value & 0xFFFFu;
    return rtc_config_end();
}

bool rtc_set_alarm(uint32_t counter_value) noexcept {
    if (!rtc_config_begin()) return false;
    RTC->ALRH = counter_value >> 16;
    RTC->ALRL = counter_value & 0xFFFFu;
    return rtc_config_end();
}

#endif // MCFRAME_STM32_DEEP_SLEEP

#if MCFRAME_STM32_DEEP_SLEEP

/// Вернуть тактирование к выбранному профилю после выхода из STOP.
/// После STOP ядро ВСЕГДА работает от HSI 8 МГц (RM0008: PLL выключен,
/// SYSCLK = HSI), поэтому:
///   * профиль LowPower — это уже нужная частота, достаточно выставить wait
///     states и убедиться, что PLL/HSE остались погашены;
///   * профиль Performance — нужно поднять HSE+PLL обратно до 72 МГц тем же
///     кодом, что и после сброса (SystemInit).
bool restore_clock_after_stop() noexcept {
    if (g_profile == platform::PowerProfile::Performance) {
        apply_performance();
        return g_sysclk_hz != 0u;
    }

    RCC->CR &= ~(RCC_CR_PLLON | RCC_CR_HSEON);
    // Частота уже низкая — можно уменьшить wait states (порядок важен).
    set_flash_latency(flash_latency_for(kHsiHz));
    RCC->CFGR &= ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2);
    SystemCoreClockUpdate();
    g_sysclk_hz = SystemCoreClock;
    return g_sysclk_hz != 0u;
}

// ─────────────────────────────── SysTick ───────────────────────────────────

/// Разрешить/запретить прерывание в NVIC.
///
/// Пишем в ISER/ICER напрямую (как platform.cpp и драйверы), а не через
/// NVIC_EnableIRQ(): во-первых, это тот же код для прошивки и для хостовой
/// модели регистров, во-вторых, ISER — не обычная память, а write-1-to-set:
/// обычная RMW-запись там сработала бы иначе, чем на железе.
void nvic_enable(uint32_t irqn) noexcept {
    NVIC->ISER[irqn >> 5u] = (1UL << (irqn & 0x1Fu));
}

void nvic_disable(uint32_t irqn) noexcept {
    NVIC->ICER[irqn >> 5u] = (1UL << (irqn & 0x1Fu));
}

/// Остановить системный тик на время сна. Возвращает прежний CTRL, чтобы
/// вернуть ровно то состояние, которое было (тик мог быть и не запущен).
uint32_t systick_freeze() noexcept {
    const uint32_t ctrl = SysTick->CTRL;
    SysTick->CTRL = 0u;
    SysTick->VAL  = 0u;
    return ctrl;
}

void systick_restore(uint32_t ctrl) noexcept {
    SysTick->VAL  = 0u;
    SysTick->CTRL = ctrl;
}

#endif // MCFRAME_STM32_DEEP_SLEEP

} // namespace

// ────────────────────────────────── Публичное ──────────────────────────────

const Config& config() noexcept { return g_cfg; }

platform::PowerProfile profile() noexcept { return g_profile; }

uint32_t sysclk_hz() noexcept { return g_sysclk_hz; }

uint32_t caps() noexcept {
    uint32_t c = static_cast<uint32_t>(platform::PowerCaps::Sleep) |
                 static_cast<uint32_t>(platform::PowerCaps::StaticProfile);
#if MCFRAME_STM32_DEEP_SLEEP
    if (g_cfg.deep_sleep_enabled) {
        c |= static_cast<uint32_t>(platform::PowerCaps::DeepSleep);
    }
#endif
    return c;
}

uint32_t set_profile(uint32_t profile_value) noexcept {
    const platform::PowerProfile want = static_cast<platform::PowerProfile>(profile_value);

    // Идемпотентность: «привести к текущему профилю» — это no-op, а не
    // повторный запуск PLL (иначе Supervisor::init() дёргал бы тактирование
    // на каждом старте приложения).
    if (want == g_profile) {
        if (g_sysclk_hz == 0u) {
            // Ещё ни разу не инициализировались: после сброса тактирование уже
            // настроено на Performance (system_stm32f10x.c: SYSCLK_FREQ_72MHz),
            // поэтому берём частоту оттуда, а не «перенастраиваем» её заново.
            g_sysclk_hz = SystemCoreClock;
        }
        if (g_sysclk_hz != 0u) return g_sysclk_hz;
    }

    switch (want) {
        case platform::PowerProfile::Performance:
            // Возврат из LowPower запрещён: HSE погашен, а периферия к этому
            // моменту уже настроена под 8 МГц (baud USART, CCR у I2C,
            // прескалер АЦП). LowPower — выбор на всю загрузку, и это честно
            // сообщается отказом, а не тихой порчей обмена по шинам.
            if (g_profile == platform::PowerProfile::LowPower) return 0u;
            apply_performance();
            return g_sysclk_hz;

        case platform::PowerProfile::LowPower:
            if (!apply_low_power()) return 0u;
            return g_sysclk_hz;
    }
    return 0u;
}

Status init(const Config& cfg) noexcept {
    g_cfg = cfg;
    g_last_error = Status::Ok;

    // Профиль частоты: применяем сразу, пока периферия и тик не настроены.
    const uint32_t hz = set_profile(static_cast<uint32_t>(cfg.profile));
    if (hz == 0u) {
        g_last_error = Status::NotSupported;
        return Status::NotSupported;
    }

#if MCFRAME_STM32_DEEP_SLEEP
    if (cfg.deep_sleep_enabled) {
        enable_backup_access();

        if (!select_rtc_clock(cfg.rtc_clock)) {
            // Источник тактирования RTC уже выбран другим (RTCSEL пишется
            // один раз) либо кварц не запустился.
            g_last_error = Status::HardwareError;
            return Status::HardwareError;
        }

        const uint32_t rtc_hz = (cfg.rtc_clock == RtcClock::Lse) ? kLseHz
                                                                 : kLsiHzNominal;
        g_rtc_prescaler = rtc_hz - 1u;   // счётчик RTC инкрементируется раз в секунду

        if (!rtc_config_begin()) {
            g_last_error = Status::HardwareError;
            return Status::HardwareError;
        }
        RTC->PRLH = 0u;
        RTC->PRLL = g_rtc_prescaler & 0xFFFFu;
        if (!rtc_config_end()) {
            g_last_error = Status::HardwareError;
            return Status::HardwareError;
        }

        if ((RTC->CRL & RTC_CRL_RSF) == 0u) {
            // Регистры RTC ещё не синхронизированы с тактированием RTC.
            g_last_error = Status::Unavailable;
            return Status::Unavailable;
        }

        (void)rtc_write_counter(0u);
        g_rtc_ready = true;
    } else {
        g_rtc_ready = false;
    }
#endif

    g_inited = true;
    return Status::Ok;
}

uint32_t deep_sleep_horizon_ms() noexcept {
#if MCFRAME_STM32_DEEP_SLEEP
    if (!g_inited || !g_cfg.deep_sleep_enabled || !g_rtc_ready) return 0u;
    return kMaxDeepSleepMs;
#else
    return 0u;
#endif
}

uint32_t deep_sleep_granularity_ms() noexcept {
#if MCFRAME_STM32_DEEP_SLEEP
    if (!g_cfg.deep_sleep_enabled || !g_rtc_ready) return 0u;
    return 1000u;   // разрешение будильника RTC на STM32F1 — одна секунда
#else
    return 0u;
#endif
}

uint32_t deep_sleep(uint32_t max_ms) noexcept {
#if MCFRAME_STM32_DEEP_SLEEP
    if (!g_inited || !g_cfg.deep_sleep_enabled || !g_rtc_ready) return 0u;
    if (max_ms == 0u) return 0u;

    // ── 1. Будильник: округляем ВВЕРХ до секунды и добавляем запас ──
    // Счётчик RTC инкрементируется раз в секунду, и мы читаем его в
    // произвольный момент внутри текущей секунды: без запаса можно встать
    // на секунду РАНЬШЕ запрошенного (то есть раньше срока задачи).
    const uint64_t want_ms = static_cast<uint64_t>(max_ms) + g_cfg.wake_slack_ms;
    uint32_t seconds = static_cast<uint32_t>((want_ms + 999u) / 1000u);
    if (seconds == 0u) seconds = 1u;

    const uint32_t before = rtc_counter();
    if (!rtc_set_alarm(before + seconds)) {
        ++g_failures;
        g_last_error = Status::HardwareError;
        return 0u;
    }

    // ── 2. Источник пробуждения: линия EXTI17 и её обработчик ──
    RTC->CRH |= RTC_CRH_ALRIE;
    EXTI->IMR |= (1u << kExtiRtcAlarmLine);
    EXTI->PR   = (1u << kExtiRtcAlarmLine);     // старый флаг не должен будить сразу
    nvic_enable(static_cast<uint32_t>(RTCAlarm_IRQn));

    // ── 3. Тик ОС на время сна останавливаем ──
    // Иначе первое же прерывание SysTick (1–10 мс) выведет ядро из STOP и
    // сна не получится. Проспанное время вернёт RTC, а монотонное время ОС
    // продвинет супервизор (OsTimer::AdvanceMs).
    const uint32_t systick_ctrl = systick_freeze();

    // ── 4. Собственно STOP ──
    PWR->CR &= ~PWR_CR_PDDS;                          // STOP, а не Standby
    if (g_cfg.low_power_regulator) PWR->CR |= PWR_CR_LPDS;
    else                          PWR->CR &= ~PWR_CR_LPDS;
    PWR->CR |= PWR_CR_CWUF;                           // снять старый WUF

    // Приведение к uint32_t: SCB_SCR_SLEEPDEEP_Msk в CMSIS имеет тип unsigned
    // long, поэтому без него -Wconversion честно ругается на сужение.
    SCB->SCR = SCB->SCR | static_cast<uint32_t>(SCB_SCR_SLEEPDEEP_Msk);
    __DSB();
    __WFI();
    SCB->SCR = SCB->SCR & ~static_cast<uint32_t>(SCB_SCR_SLEEPDEEP_Msk);

    // ── 5. Выход: снять флаги, снять маску линии, вернуть тактирование ──
    PWR->CR |= PWR_CR_CWUF;
    EXTI->PR = (1u << kExtiRtcAlarmLine);
    EXTI->IMR &= ~(1u << kExtiRtcAlarmLine);
    RTC->CRL &= ~RTC_CRL_ALRF;
    nvic_disable(static_cast<uint32_t>(RTCAlarm_IRQn));

    // После STOP ядро работает от HSI: возвращаем профиль (для LowPower это
    // пустая операция — частота уже HSI, но latency и PLL выставляются те же).
    if (!restore_clock_after_stop()) {
        ++g_failures;
        g_last_error = Status::Fault;
    }
    systick_restore(systick_ctrl);

    // ── 6. Измеренное время сна ──
    const uint32_t after = rtc_counter();
    const uint32_t seconds_slept = (after >= before) ? (after - before) : 0u;
    const uint32_t slept_ms = seconds_slept * 1000u;

    if (seconds_slept == 0u) {
        // Проснулись не по будильнику (внешнее прерывание) или RTC не успел
        // отсчитать секунду — честно сообщаем «спали неизвестно сколько».
        ++g_failures;
        g_last_error = Status::Timeout;
        return 0u;
    }

    ++g_entries;
    g_total_ms += slept_ms;
    return slept_ms;
#else
    (void)max_ms;
    return 0u;
#endif
}

uint32_t deep_sleep_entries() noexcept { return g_entries; }

uint32_t deep_sleep_total_ms() noexcept { return g_total_ms; }

uint32_t deep_sleep_failures() noexcept { return g_failures; }

Status last_error() noexcept { return g_last_error; }

#if MCFRAME_STM32_DEEP_SLEEP

/// Обработчик будильника RTC. Выход из STOP происходит ДО него: здесь только
/// снимаются флаги, чтобы прерывание не «дребезжало» и не мешало измерению.
void on_alarm_irq() noexcept {
    EXTI->PR = (1u << kExtiRtcAlarmLine);
    RTC->CRL &= ~RTC_CRL_ALRF;
}

#endif // MCFRAME_STM32_DEEP_SLEEP

} // namespace embed::stm32::power
