// platforms/mock/platform_watchdog_mock.hpp
#pragma once
// ---------------------------------------------------------------------------
// Программная модель сторожевого таймера для сборки/тестов на ПК.
//
// Модель воспроизводит ровно те свойства железа, от которых зависит политика
// сервиса (os/watchdog):
//   * дискретность таймаута: closest_timeout_ms округляет вверх до «ступени»
//     (как предделитель IWDG), поэтому тест видит, что запас считается по
//     ФАКТИЧЕСКОМУ таймауту, а не по запрошенному;
//   * необратимость: stop() отказывает, пока «железо» не помечено как
//     остановистое (у IWDG такой возможности нет вообще);
//   * резервные регистры: массив, «переживающий» вызов reboots() — именно так
//     проверяется посмертная диагностика (в том числе счётчик загрузок).
//
// Чего модель НЕ делает: она не сбрасывает процесс. Вместо этого она считает,
// сколько раз железо сработало бы (expired()), — тест проверяет, что сервис
// ПЕРЕСТАЛ кормить таймер, а не «перезагрузил хост».
// ---------------------------------------------------------------------------
#include "../../os/iplatform.hpp"

namespace embed::mock_platform {

struct WatchdogModel {
    // ── «Железо» ──
    uint32_t caps =
        static_cast<uint32_t>(platform::WatchdogCaps::Hardware) |
        static_cast<uint32_t>(platform::WatchdogCaps::ResetCause) |
        static_cast<uint32_t>(platform::WatchdogCaps::BackupNote);

    /// Ступень дискретности таймаута, мс (как предделитель IWDG).
    uint32_t timeout_step_ms = 100u;
    /// Верхний предел таймаута (0 — без предела).
    uint32_t max_timeout_ms  = 20000u;
    /// Старт «железа» разрешён (false — моделируем отказ запуска).
    bool     start_ok = true;

    /// Причина последнего сброса, которую вернёт read_reset_cause().
    uint32_t reset_cause_flags = 0u;
    /// Сколько резервных регистров доступно.
    static constexpr uint32_t kNoteRegs = 10u;

    // ── Наблюдение ──
    bool     started      = false;
    uint32_t timeout_ms   = 0u;   ///< фактически установленный таймаут
    uint32_t start_calls  = 0u;
    uint32_t kick_calls   = 0u;
    uint32_t stop_calls   = 0u;
    uint32_t cause_reads  = 0u;
    uint16_t notes[kNoteRegs]{};

    /// Сколько миллисекунд прошло с последнего кормления — считает тест,
    /// продвигая время сам (advance()).
    uint32_t since_kick_ms = 0u;
    /// Сколько раз таймер «сработал бы» (превышен таймаут без кормления).
    uint32_t expired = 0u;

    /// Продвинуть «время железа» на ms: если таймер запущен и его не кормили
    /// дольше установленного таймаута — это сброс устройства.
    void advance(uint32_t ms) noexcept {
        if (!started) return;
        since_kick_ms += ms;
        if (timeout_ms != 0u && since_kick_ms >= timeout_ms) {
            ++expired;
            since_kick_ms = 0u;      // после сброса таймер начинается заново
            started = false;         // и «железо» перезапустится заново
        }
    }

    /// Сброс «железа» между тестами. resetNotes == false сохраняет резервные
    /// регистры (они переживают сброс МКУ — в этом весь смысл).
    void reset(bool resetNotes = true) noexcept {
        started = false;
        timeout_ms = 0u;
        start_calls = kick_calls = stop_calls = cause_reads = 0u;
        since_kick_ms = 0u;
        expired = 0u;
        if (resetNotes) {
            for (auto& n : notes) n = 0u;
        }
    }
};

/// Единственный экземпляр модели (пишет платформа, читает тест).
WatchdogModel& watchdog_model() noexcept;

} // namespace embed::mock_platform
