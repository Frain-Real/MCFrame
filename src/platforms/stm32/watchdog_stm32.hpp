// platforms/stm32/watchdog_stm32.hpp
#pragma once
// ---------------------------------------------------------------------------
// СТОРОЖЕВОЙ ТАЙМЕР STM32F103: независимый (IWDG), причина последнего сброса
// и посмертная отметка в резервных регистрах.
//
// ПОЧЕМУ IWDG, А НЕ WWDG
//   * IWDG тактируется от LSI и продолжает считать в Stop/Standby, то есть
//     защищает УСНУВШЕЕ устройство — единственный случай, который для
//     энергосберегающего прибора и опасен;
//   * WWDG тактируется от PCLK1: в Stop он останавливается (защиты нет), а
//     смена профиля частоты меняет его период. Роль «окна» играет
//     Participant::min_interval_ms в ядре (см. os/watchdog/watchdog.hpp).
//
// ЧТО НУЖНО ЗНАТЬ ПРО IWDG
//   * он не останавливается: после 0xCCCC в IWDG_KR выключить его можно только
//     сбросом питания. Поэтому сервис запускает его в самом конце запуска
//     приложения (watchdog::Service::start), а не в init();
//   * LSI имеет разброс 30–60 кГц (DS5319), поэтому таймаут считаетcя по
//     МАКСИМАЛЬНОЙ частоте: обещанный срок — гарантированный минимум, на
//     «типичных» 40 кГц он окажется в 1.5 раза длиннее. Обратный выбор
//     (считать по 40 кГц) дал бы сбросы раньше срока.
// ---------------------------------------------------------------------------

#include "stm32_regs.hpp"
#include "../../os/common.hpp"

#include <cstdint>

namespace embed::stm32::watchdog {

/// См. platform::WatchdogOps.
uint32_t caps() noexcept;
uint32_t closest_timeout_ms(uint32_t requested_ms) noexcept;
bool     start(uint32_t timeout_ms) noexcept;
void     kick() noexcept;
bool     stop() noexcept;
bool     note_write(uint32_t index, uint16_t value) noexcept;
uint16_t note_read(uint32_t index) noexcept;
uint32_t read_reset_cause() noexcept;

// ───────────────────────── Наблюдение (диагностика) ─────────────────────────
uint32_t kicks() noexcept;        ///< сколько раз кормили
uint32_t timeout_ms() noexcept;   ///< фактически установленный таймаут
bool     running() noexcept;      ///< таймер запущен (необратимо)
bool     last_start_failed() noexcept;

// ───────────────────────── Резервные регистры ───────────────────────────────
/// Сколько резервных регистров доступно (STM32F103RB: 10).
constexpr uint32_t kNoteRegs = 10u;

} // namespace embed::stm32::watchdog
