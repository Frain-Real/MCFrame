// tests/host/test_stm32_watchdog.cpp
// ---------------------------------------------------------------------------
// СТОРОЖЕВОЙ ТАЙМЕР STM32 НА ПК: последовательность работы с IWDG, расчёт
// таймаута по разбросу LSI, резервные регистры и причина сброса.
//
// ЗАЧЕМ ЭТОТ НАБОР
// Ошибка в настройке IWDG не видна в отладчике: она проявляется сбросом
// устройства через НЕПРАВИЛЬНЫЙ срок — например, через 273 мс вместо 6 с,
// если забыть, что PR/RLR пишутся только после ключа 0x5555, или если не
// дождаться снятия PVU/RVU. Модель регистров воспроизводит оба этих свойства,
// поэтому тест проверяет не «код дошёл до строки», а ФАКТ настройки.
//
// Отдельная тема — расчёт таймаута: LSI имеет разброс 30–60 кГц, и обещанный
// срок обязан быть ГАРАНТИРОВАННЫМ минимумом. Здесь это проверяется числом:
// при 60 кГц (худший случай) запрограммированный таймаут не меньше
// запрошенного, а при 40 кГц — заметно больше (то есть сброс не раньше срока).
// ---------------------------------------------------------------------------
#include "stm32_regs_fake.hpp"

#include "watchdog_stm32.hpp"

#include <cstdio>

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
using embed::test::g_bkp;
using embed::test::g_iwdg;
using embed::test::g_pwr;
using embed::test::g_rcc;

namespace wd = embed::stm32::watchdog;

// Частоты LSI по даташиту: 30–60 кГц, типовое 40 кГц.
constexpr uint32_t kLsiMin = 30000u;
constexpr uint32_t kLsiTyp = 40000u;
constexpr uint32_t kLsiMax = 60000u;

void resetHardware() {
    test::fakeReset();
}

// ─────────────────────────── Расчёт таймаута ────────────────────────────────

void test_timeout_math() {
    (void)std::puts("расчёт таймаута по разбросу LSI:");
    resetHardware();

    check_eq<unsigned long long>("нулевой запрос -> 0", wd::closest_timeout_ms(0u), 0ull);

    // Главное свойство: обещанный срок — гарантированный минимум на всех
    // частотах LSI. Проверяем на наборе значений, включая границы диапазона.
    const uint32_t requests[] = {1u, 10u, 100u, 500u, 1000u, 2333u, 5000u, 10000u, 17400u};
    bool all_ok = true;
    bool all_reachable = true;
    for (uint32_t r : requests) {
        const uint32_t got = wd::closest_timeout_ms(r);
        if (got == 0u) all_reachable = false;
        else if (got < r) all_ok = false;
    }
    check("любой достижимый таймаут не меньше запрошенного", all_ok);
    check("до ~17 с любой срок достижим", all_reachable);

    // Больше предела независимого таймера он не умеет — честный отказ, а не
    // «поставим что получится».
    check_eq<unsigned long long>("слишком длинный срок -> 0 (не умеет)",
          wd::closest_timeout_ms(60000u), 0ull);

    // Дискретность: 1000 мс выражается точно, 1010 — с округлением вверх.
    check_eq<unsigned long long>("1000 мс выражается точно",
          wd::closest_timeout_ms(1000u), 1000ull);
    check("1010 мс округляется вверх", wd::closest_timeout_ms(1010u) >= 1010u);
}

// ────────────────────────────── Настройка IWDG ──────────────────────────────

void test_start_sequence() {
    (void)std::puts("последовательность настройки IWDG:");
    resetHardware();

    const uint32_t requested = 6000u;
    const uint32_t timeout = wd::closest_timeout_ms(requested);
    check("срок достижим", timeout > 0u);

    check("start() == true", wd::start(timeout));
    check("таймер запущен (ключ 0xCCCC)", g_iwdg.started());
    check("запись PR/RLR разрешена (ключ 0x5555)", g_iwdg.write_access());
    check("счётчик перезагружен (ключ 0xAAAA)", g_iwdg.reload_keys() >= 1u);

    // Флаги обновления обязаны быть ДОЖДАЛЫ: модель показывает их выставленными
    // на первом чтении SR, поэтому одного чтения недостаточно.
    check("ожидание снятия PVU/RVU выполнено", g_iwdg.sr_polls() >= 2u);

    // ── Срок ──
    // Запрограммированная пара (PR, RLR) при ХУДШЕЙ частоте LSI (60 кГц) даёт
    // не меньше запрошенного…
    const uint32_t worst_case = g_iwdg.timeout_ms_at(kLsiMax);
    check("при 60 кГц таймаут не меньше запрошенного", worst_case >= requested);
    // …а при типовой и минимальной частоте — заметно больше: сброс произойдёт
    // ПОЗЖЕ срока, но никогда раньше. Обратный выбор частоты дал бы ложные
    // сбросы.
    check("при 40 кГц таймаут длиннее (сброс не раньше срока)",
          g_iwdg.timeout_ms_at(kLsiTyp) >= worst_case);
    check("при 30 кГц таймаут ещё длиннее",
          g_iwdg.timeout_ms_at(kLsiMin) >= g_iwdg.timeout_ms_at(kLsiTyp));

    // ── LSI включён и дождались готовности ──
    check("LSI включён", g_rcc.lsi_enabled());
    check("резервная область открыта для записи (DBP)",
          (g_pwr.cr() & PWR_CR_DBP) != 0u);

    // ── Кормление ──
    const uint32_t reloads = g_iwdg.reload_keys();
    wd::kick();
    check_eq<unsigned long long>("kick() перезагружает счётчик",
          g_iwdg.reload_keys(), reloads + 1ull);
    check_eq<unsigned long long>("кормления посчитаны", wd::kicks(), reloads + 1ull);

    // ── Повторный запуск и остановка ──
    check("повторный start() не перезапускает таймер", wd::start(timeout));
    check("таймер по-прежнему работает", wd::running());
    check("остановка невозможна (IWDG не выключается)", !wd::stop());
    check("в возможностях нет Stoppable",
          (wd::caps() & static_cast<uint32_t>(platform::WatchdogCaps::Stoppable)) == 0u);
    check("заявлены Hardware/ResetCause/BackupNote",
          (wd::caps() & static_cast<uint32_t>(platform::WatchdogCaps::Hardware)) != 0u &&
          (wd::caps() & static_cast<uint32_t>(platform::WatchdogCaps::ResetCause)) != 0u &&
          (wd::caps() & static_cast<uint32_t>(platform::WatchdogCaps::BackupNote)) != 0u);
}

// ──────────────────── Ключ записи: без него настройка теряется ─────────────

void test_write_protection_is_real() {
    (void)std::puts("защита записи PR/RLR (проверка самой модели):");
    resetHardware();

    // Пишем PR/RLR БЕЗ ключа 0x5555 — модель обязана проигнорировать запись,
    // как железо. Иначе проверки выше ничего не доказывали бы: они
    // «проходили» бы и с кодом, забывшим ключ.
    IWDG->PR  = 6u;
    IWDG->RLR = 4095u;
    check_eq<unsigned long long>("без ключа прескалер не записан", g_iwdg.prescaler(), 0ull);
    check_eq<unsigned long long>("без ключа перезагрузка не записана", g_iwdg.reload(), 0ull);

    // С ключом запись проходит — то есть дело было именно в ключе.
    IWDG->KR  = 0x5555u;               // IWDG_KR_KEY_ENABLE
    IWDG->PR  = 4u;
    IWDG->RLR = 1000u;
    check_eq<unsigned long long>("с ключом прескалер записан", g_iwdg.prescaler(), 4ull);
    check_eq<unsigned long long>("с ключом перезагрузка записана", g_iwdg.reload(), 1000ull);
}

// ───────────────────────── Резервные регистры (BKP) ─────────────────────────

void test_backup_notes() {
    (void)std::puts("посмертная отметка в резервных регистрах:");
    resetHardware();

    check_eq<unsigned long long>("до записи регистр пуст", wd::note_read(2u), 0ull);
    check("запись выполнена", wd::note_write(2u, 0x1234u));
    check_eq<unsigned long long>("значение читается", wd::note_read(2u), 0x1234ull);
    check("DBP выставлен автоматически", (g_pwr.cr() & PWR_CR_DBP) != 0u);

    // Границы: индекса вне доступных регистров не существует — отказ, а не
    // «запись мимо».
    check("индекс за границей -> false", !wd::note_write(wd::kNoteRegs, 1u));
    check_eq<unsigned long long>("чтение за границей -> 0", wd::note_read(99u), 0ull);

    // Отметка должна ПЕРЕЖИТЬ сброс МКУ: модель умеет сбросить периферию и
    // ядро, сохранив резервную область, — как это делает настоящий сброс.
    test::fakeResetKeepBackup();
    check_eq<unsigned long long>("отметка пережила сброс", wd::note_read(2u), 0x1234ull);
    check("причина последнего сброса — включение питания",
          (wd::read_reset_cause() &
           static_cast<uint32_t>(platform::ResetCause::PowerOn)) != 0u);

    // Платформа обязана уметь записать отметку сразу после сброса, когда
    // доступ к резервной области ещё закрыт.
    check("запись сразу после сброса", wd::note_write(3u, 0xBEEFu));
    check_eq<unsigned long long>("и она читается", wd::note_read(3u), 0xBEEFull);
}

// ───────────────────────── Причина последнего сброса ────────────────────────

void test_reset_cause() {
    (void)std::puts("причина последнего сброса:");
    resetHardware();

    // Сброс от независимого сторожа + включение питания.
    g_rcc.regs.CSR = RCC_CSR_IWDGRSTF | RCC_CSR_PORRSTF;

    const uint32_t cause = wd::read_reset_cause();
    check("сброс от сторожа распознан",
          (cause & static_cast<uint32_t>(platform::ResetCause::IndependentWatchdog)) != 0u);
    check("включение питания распознано",
          (cause & static_cast<uint32_t>(platform::ResetCause::PowerOn)) != 0u);
    check("другие источники не приписаны",
          (cause & static_cast<uint32_t>(platform::ResetCause::Software)) == 0u);
    check("бит RMVF не хранится в CSR", (g_rcc.regs.CSR & RCC_CSR_RMVF) == 0u);

    // Чтение снимает флаги: второй раз причина пуста — поэтому ядро читает её
    // ровно один раз (см. Service::init).
    check_eq<unsigned long long>("повторное чтение не находит причин",
          static_cast<unsigned long long>(wd::read_reset_cause()), 0ull);

    // Программный сброс распознаётся отдельно.
    g_rcc.regs.CSR = RCC_CSR_SFTRSTF;
    check("программный сброс распознан",
          (wd::read_reset_cause() &
           static_cast<uint32_t>(platform::ResetCause::Software)) != 0u);
}

} // namespace

int main() {
    (void)std::puts("=== сторожевой таймер STM32 на модели регистров ===");

    test_timeout_math();
    test_write_protection_is_real();
    test_start_sequence();
    test_backup_notes();
    test_reset_cause();

    (void)std::printf("\nвсего проверок: %d, провалено: %d\n", g_total, g_failed);
    if (g_failed != 0) (void)std::puts("FAIL");
    return (g_failed == 0) ? 0 : 1;
}
