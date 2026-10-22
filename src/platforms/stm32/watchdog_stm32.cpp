// platforms/stm32/watchdog_stm32.cpp
// ---------------------------------------------------------------------------
// Независимый сторожевой таймер STM32F103 (IWDG), причина последнего сброса и
// отметки в резервных регистрах (BKP).
//
// Регистры — ТОЛЬКО через stm32_regs.hpp, поэтому файл собирается и в
// прошивку, и в хостовый набор на модели регистров (tests/host): проверяется
// последовательность работы с IWDG_KR/IWDG_SR (запись разрешена только после
// 0x5555, значения применяются только после снятия PVU/RVU) — именно на этом
// месте ошибаются чаще всего, а цена ошибки — таймер с таймаутом по умолчанию
// вместо заданного.
// ---------------------------------------------------------------------------
#include "watchdog_stm32.hpp"

namespace embed::stm32::watchdog {

namespace {

// Ключи IWDG_KR (RM0008 §17.4.1). В CMSIS есть только 0xFFFF, поэтому
// остальные названы здесь: это часть протокола общения с железом.
constexpr uint32_t kKeyReload   = 0x0000AAAAu;   ///< перезагрузить счётчик (кормление)
constexpr uint32_t kKeyEnable   = 0x00005555u;   ///< разрешить запись PR/RLR
constexpr uint32_t kKeyStart    = 0x0000CCCCu;   ///< запустить таймер

/// Частота LSI. Разброс по даташиту 30–60 кГц, типовое 40 кГц.
constexpr uint32_t kLsiHzTypical = 40000u;
/// Для РАСЧЁТА таймаута берём максимум: обещанный срок должен быть
/// гарантированным минимумом (см. шапку заголовка).
constexpr uint32_t kLsiHzMax     = 60000u;

/// Предделители IWDG: 4, 8, 16, 32, 64, 128, 256 (PR = 0..6).
constexpr uint32_t kPrescalerCount = 7u;
constexpr uint32_t kReloadMax      = 0x0FFFu;   ///< RLR 12-битный

/// Ожидание снятия флагов обновления. Ждать вечно нельзя: если IWDG не
/// отвечает, приложение обязано получить отказ, а не встать.
constexpr uint32_t kSpinLimit = 0x000FFFFFu;

bool spin_until_flags_clear() noexcept {
    for (uint32_t i = 0u; i < kSpinLimit; ++i) {
        if ((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) == 0u) return true;
    }
    return false;
}

uint32_t g_timeout_ms = 0u;
uint32_t g_kicks      = 0u;
bool     g_running    = false;
bool     g_start_failed = false;

/// Разрешить доступ к резервной области: без DBP запись в BKP игнорируется.
void enable_backup_access() noexcept {
    RCC->APB1ENR |= RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
    PWR->CR      |= PWR_CR_DBP;
}

/// Записать резервный регистр по индексу. BKP_DR1..DR10 (medium density).
///
/// Развёрнутый switch, а не арифметика по указателю: во-первых, поля структуры
/// CMSIS разделены половиной слова (арифметика была бы непереносимой), во-вторых,
/// на хостовой модели регистры — прокси с побочными эффектами, и обращаться к
/// ним нужно по имени. За границей массива — отказ, а не «запись мимо».
bool note_write_reg(uint32_t index, uint16_t value) noexcept {
    switch (index) {
        case 0u: BKP->DR1  = value; return true;
        case 1u: BKP->DR2  = value; return true;
        case 2u: BKP->DR3  = value; return true;
        case 3u: BKP->DR4  = value; return true;
        case 4u: BKP->DR5  = value; return true;
        case 5u: BKP->DR6  = value; return true;
        case 6u: BKP->DR7  = value; return true;
        case 7u: BKP->DR8  = value; return true;
        case 8u: BKP->DR9  = value; return true;
        case 9u: BKP->DR10 = value; return true;
        default: return false;
    }
}

uint16_t note_read_reg(uint32_t index) noexcept {
    switch (index) {
        case 0u: return static_cast<uint16_t>(BKP->DR1);
        case 1u: return static_cast<uint16_t>(BKP->DR2);
        case 2u: return static_cast<uint16_t>(BKP->DR3);
        case 3u: return static_cast<uint16_t>(BKP->DR4);
        case 4u: return static_cast<uint16_t>(BKP->DR5);
        case 5u: return static_cast<uint16_t>(BKP->DR6);
        case 6u: return static_cast<uint16_t>(BKP->DR7);
        case 7u: return static_cast<uint16_t>(BKP->DR8);
        case 8u: return static_cast<uint16_t>(BKP->DR9);
        case 9u: return static_cast<uint16_t>(BKP->DR10);
        default: return 0u;
    }
}

} // namespace

// ────────────────────────────── Возможности ─────────────────────────────────

uint32_t caps() noexcept {
    return static_cast<uint32_t>(platform::WatchdogCaps::Hardware) |
           static_cast<uint32_t>(platform::WatchdogCaps::ResetCause) |
           static_cast<uint32_t>(platform::WatchdogCaps::BackupNote);
    // Stoppable НЕ заявляем: IWDG не останавливается — это свойство железа,
    // а не недоработка, и ядро обязано об этом знать.
}

uint32_t closest_timeout_ms(uint32_t requested_ms) noexcept {
    if (requested_ms == 0u) return 0u;

    // Таймаут IWDG = (4 × 2^PR × RLR) / f_LSI.
    // Для каждого предделителя считаем нужный RLR и выбираем ПЕРВЫЙ (самый
    // мелкий предделитель), который укладывается в 12 бит: он даёт лучшее
    // разрешение.
    for (uint32_t pr = 0u; pr < kPrescalerCount; ++pr) {
        const uint64_t divider = 4ull << pr;                       // 4 × 2^PR
        // RLR = ceil(requested_ms × f_LSI / (1000 × divider))
        const uint64_t num = static_cast<uint64_t>(requested_ms) * kLsiHzMax;
        const uint64_t den = 1000ull * divider;
        const uint64_t rlr = (num + den - 1ull) / den;
        if (rlr == 0ull) continue;
        if (rlr > kReloadMax) continue;                            // не влезает в RLR

        // Фактический таймаут по той же максимальной частоте: он и есть
        // гарантированный минимум.
        const uint64_t actual = (den * rlr) / kLsiHzMax;
        const uint64_t rounded = (actual == 0ull) ? 1ull : actual;
        return static_cast<uint32_t>(rounded);
    }

    // Больше ~17.5 с (при 60 кГц) независимый таймер не умеет: вернуть
    // меньше запрошенного нельзя, поэтому честный отказ.
    return 0u;
}

// ──────────────────────────────── Запуск ────────────────────────────────────

bool start(uint32_t timeout_ms) noexcept {
    if (g_running) return true;               // повторный запуск — уже запущен
    if (timeout_ms == 0u) return false;

    // LSI включаем сами: IWDG тактируется от него, а ждать готовности нужно
    // ДО запуска, иначе первые такты таймера пойдут от ещё не стабильного
    // генератора.
    RCC->CSR |= RCC_CSR_LSION;
    bool lsi_ready = false;
    for (uint32_t i = 0u; i < kSpinLimit; ++i) {
        if ((RCC->CSR & RCC_CSR_LSIRDY) != 0u) { lsi_ready = true; break; }
    }
    if (!lsi_ready) { g_start_failed = true; return false; }
    (void)enable_backup_access();

    // ── Последовательность RM0008 §17.3.2 ──
    // 1. Запуск: с этого момента таймер считает по значениям по умолчанию
    //    (PR = 0, RLR = 4095), то есть даёт нам время на настройку.
    IWDG->KR = kKeyStart;
    g_running = true;                          // необратимо — отмечаем сразу

    // 2. Разрешить запись PR/RLR.
    IWDG->KR = kKeyEnable;

    // 3. Дождаться, пока железо применит предыдущие значения.
    if (!spin_until_flags_clear()) { g_start_failed = true; return false; }

    // 4. Найти предделитель и значение перезагрузки под запрошенный таймаут.
    //    Повторяем расчёт closest_timeout_ms(), чтобы получить САМУ пару
    //    (PR, RLR), а не только срок.
    uint32_t chosen_pr = 0u, chosen_rlr = 0u;
    for (uint32_t pr = 0u; pr < kPrescalerCount; ++pr) {
        const uint64_t divider = 4ull << pr;
        const uint64_t num = static_cast<uint64_t>(timeout_ms) * kLsiHzMax;
        const uint64_t den = 1000ull * divider;
        const uint64_t rlr = (num + den - 1ull) / den;
        if (rlr == 0ull || rlr > kReloadMax) continue;
        chosen_pr  = pr;
        chosen_rlr = static_cast<uint32_t>(rlr);
        break;
    }
    if (chosen_rlr == 0u) { g_start_failed = true; return false; }

    IWDG->PR  = chosen_pr;
    if (!spin_until_flags_clear()) { g_start_failed = true; return false; }
    IWDG->RLR = chosen_rlr;
    if (!spin_until_flags_clear()) { g_start_failed = true; return false; }

    // 5. Перезагрузить счётчик новыми значениями и покормить.
    IWDG->KR = kKeyReload;
    ++g_kicks;
    g_timeout_ms = timeout_ms;
    return true;
}

void kick() noexcept {
    IWDG->KR = kKeyReload;
    ++g_kicks;
}

bool stop() noexcept {
    // У IWDG такой возможности нет: счётчик можно обнулить, но не выключить.
    return false;
}

// ──────────────────────── Резервные регистры (BKP) ──────────────────────────

bool note_write(uint32_t index, uint16_t value) noexcept {
    if (index >= kNoteRegs) return false;
    // Без доступа к резервной области запись молча теряется — поэтому
    // разрешаем его сами, а не надеемся на порядок инициализации.
    (void)enable_backup_access();
    return note_write_reg(index, value);
}

uint16_t note_read(uint32_t index) noexcept {
    if (index >= kNoteRegs) return 0u;
    // Чтение тоже требует включённых PWR/BKP: иначе вернётся 0 и посмертная
    // метка «исчезнет» ровно в том месте, где на неё рассчитывают.
    (void)enable_backup_access();
    return note_read_reg(index);
}

// ──────────────────────── Причина последнего сброса ─────────────────────────

uint32_t read_reset_cause() noexcept {
    const uint32_t csr = RCC->CSR;
    uint32_t cause = 0u;

    if ((csr & RCC_CSR_PORRSTF)  != 0u) cause |= static_cast<uint32_t>(platform::ResetCause::PowerOn);
    if ((csr & RCC_CSR_PINRSTF)  != 0u) cause |= static_cast<uint32_t>(platform::ResetCause::Pin);
    if ((csr & RCC_CSR_SFTRSTF)  != 0u) cause |= static_cast<uint32_t>(platform::ResetCause::Software);
    if ((csr & RCC_CSR_IWDGRSTF) != 0u) cause |= static_cast<uint32_t>(platform::ResetCause::IndependentWatchdog);
    if ((csr & RCC_CSR_WWDGRSTF) != 0u) cause |= static_cast<uint32_t>(platform::ResetCause::WindowWatchdog);
    if ((csr & RCC_CSR_LPWRRSTF) != 0u) cause |= static_cast<uint32_t>(platform::ResetCause::LowPower);

    // Флаги снимаются записью RMVF — иначе следующий сброс «унаследует» старую
    // причину. Читаем ровно один раз (ядро это и делает: см. Service::init).
    RCC->CSR |= RCC_CSR_RMVF;
    return cause;
}

// ─────────────────────────────── Наблюдение ─────────────────────────────────

uint32_t kicks() noexcept { return g_kicks; }
uint32_t timeout_ms() noexcept { return g_timeout_ms; }
bool     running() noexcept { return g_running; }
bool     last_start_failed() noexcept { return g_start_failed; }

} // namespace embed::stm32::watchdog
