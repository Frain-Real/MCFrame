// platforms/mock/platform_watchdog.cpp
// ---------------------------------------------------------------------------
// Реализация platform::WatchdogOps для mock-сборки: модель сторожевого
// таймера (см. platform_watchdog_mock.hpp) вместо железа.
//
// Функции обязаны вести себя так же честно, как железо: округлять таймаут
// ВВЕРХ до ступени предделителя, отказывать в остановке неостанавливаемого
// таймера и НЕ «перезагружать» хост — срабатывание только считается
// (WatchdogModel::expired), потому что тест проверяет политику сервиса, а не
// ядро операционной системы хоста.
// ---------------------------------------------------------------------------
#include "platform_watchdog_mock.hpp"

namespace embed::mock_platform {

WatchdogModel& watchdog_model() noexcept {
    static WatchdogModel m;
    return m;
}

} // namespace embed::mock_platform

namespace embed::platform {

namespace {

using embed::mock_platform::WatchdogModel;
using embed::mock_platform::watchdog_model;

uint32_t mock_wd_caps() OS_PLATFORM_FN {
    return watchdog_model().caps;
}

uint32_t mock_wd_closest_timeout_ms(uint32_t requested_ms) OS_PLATFORM_FN {
    const WatchdogModel& m = watchdog_model();
    if (requested_ms == 0u || m.timeout_step_ms == 0u) return 0u;

    // Вверх, а не к ближайшему: таймер, срабатывающий РАНЬШЕ запрошенного
    // срока, не даёт сервису записать причину просрочки.
    const uint32_t steps = (requested_ms + m.timeout_step_ms - 1u) / m.timeout_step_ms;
    const uint64_t rounded = static_cast<uint64_t>(steps) * m.timeout_step_ms;

    if (m.max_timeout_ms != 0u && rounded > m.max_timeout_ms) return 0u;
    return static_cast<uint32_t>(rounded);
}

bool mock_wd_start(uint32_t timeout_ms) OS_PLATFORM_FN {
    WatchdogModel& m = watchdog_model();
    ++m.start_calls;
    if (!m.start_ok || timeout_ms == 0u) return false;

    m.started    = true;
    m.timeout_ms = timeout_ms;
    m.since_kick_ms = 0u;
    return true;
}

void mock_wd_kick() OS_PLATFORM_FN {
    WatchdogModel& m = watchdog_model();
    ++m.kick_calls;
    m.since_kick_ms = 0u;
}

bool mock_wd_stop() OS_PLATFORM_FN {
    WatchdogModel& m = watchdog_model();
    ++m.stop_calls;

    // Как IWDG: не останавливается, если платформа не заявила обратное.
    if ((m.caps & static_cast<uint32_t>(WatchdogCaps::Stoppable)) == 0u) return false;
    m.started = false;
    return true;
}

bool mock_wd_note_write(uint32_t index, uint16_t value) OS_PLATFORM_FN {
    WatchdogModel& m = watchdog_model();
    if ((m.caps & static_cast<uint32_t>(WatchdogCaps::BackupNote)) == 0u) return false;
    if (index >= WatchdogModel::kNoteRegs) return false;
    m.notes[index] = value;
    return true;
}

uint16_t mock_wd_note_read(uint32_t index) OS_PLATFORM_FN {
    WatchdogModel& m = watchdog_model();
    if (index >= WatchdogModel::kNoteRegs) return 0u;
    return m.notes[index];
}

uint32_t mock_wd_read_reset_cause() OS_PLATFORM_FN {
    WatchdogModel& m = watchdog_model();
    ++m.cause_reads;
    const uint32_t flags = m.reset_cause_flags;
    // RCC_CSR на железе сбрасывает флаги при чтении — модель делает то же,
    // иначе «прочитал дважды» дало бы другой ответ, чем на плате.
    m.reset_cause_flags = 0u;
    return flags;
}

const WatchdogOps kMockWatchdogOps{
    /* caps                */ &mock_wd_caps,
    /* closest_timeout_ms  */ &mock_wd_closest_timeout_ms,
    /* start               */ &mock_wd_start,
    /* kick                */ &mock_wd_kick,
    /* stop                */ &mock_wd_stop,
    /* note_write          */ &mock_wd_note_write,
    /* note_read           */ &mock_wd_note_read,
    /* read_reset_cause    */ &mock_wd_read_reset_cause,
};

} // namespace

const WatchdogOps& mock_watchdog_ops() noexcept { return kMockWatchdogOps; }

} // namespace embed::platform
