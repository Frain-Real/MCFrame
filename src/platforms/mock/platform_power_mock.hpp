// platforms/mock/platform_power_mock.hpp
#pragma once
// ---------------------------------------------------------------------------
// Программная модель энергосбережения для сборки/тестов на ПК.
//
// ЗАЧЕМ ОТДЕЛЬНАЯ МОДЕЛЬ, А НЕ ЗАГЛУШКИ
// Политика супервизора (когда спать, когда нельзя, что делать после сна)
// проверяется на ПК именно потому, что на железе цена ошибки — зависание:
// уход в STOP без вооружённого источника пробуждения не лечится ничем, кроме
// кнопки Reset. Модель позволяет задать все «неудобные» состояния явно:
//   * источника пробуждения нет (deep_horizon_ms == 0);
//   * будильник грубый (deep_granularity_ms == 1000);
//   * платформа отказалась входить в сон (deep_enter == false);
//   * сон оказался длиннее/короче запрошенного (deep_slept_ms).
//
// Модель НЕ имитирует железо: она фиксирует ПОРЯДОК вызовов и их аргументы,
// чтобы тест проверял решение супервизора, а не регистры (для регистров есть
// отдельная модель: tests/host/stm32_regs_fake.hpp).
// ---------------------------------------------------------------------------
#include "../../os/iplatform.hpp"

namespace embed::mock_platform {

struct PowerModel {
    // ── Что «умеет» платформа ──
    uint32_t caps =
        static_cast<uint32_t>(platform::PowerCaps::Sleep) |
        static_cast<uint32_t>(platform::PowerCaps::DeepSleep) |
        static_cast<uint32_t>(platform::PowerCaps::StaticProfile);

    uint32_t performance_hz = 72000000u;   ///< профиль Performance
    uint32_t low_power_hz   = 8000000u;    ///< профиль LowPower
    uint32_t sysclk_hz      = 72000000u;   ///< текущая частота ядра

    // ── Источник пробуждения ──
    /// Сколько платформа гарантирует проспать. 0 — источник не вооружён,
    /// глубокий сон недоступен (именно так выглядит ошибочная конфигурация
    /// RTC/EXTI на реальном МКУ).
    uint32_t deep_horizon_ms     = 0u;
    /// Гранулярность будильника. 1000 мс — как RTC STM32F1.
    uint32_t deep_granularity_ms = 1000u;

    // ── Поведение при входе в сон ──
    /// false — платформа отказала во входе (deep_sleep вернул 0), хотя
    /// горизонт гарантирован. Так выглядит, например, незапустившийся RTC.
    bool     deep_enter    = true;
    /// 0 — «спали ровно запрошенное время».
    uint32_t deep_slept_ms = 0u;

    // ── Наблюдение ──
    uint32_t idle_calls      = 0u;   ///< вызовов Ops::idle (Sleep/WFI)
    uint32_t deep_calls      = 0u;   ///< попыток входа в DeepSleep
    uint32_t last_request_ms = 0u;   ///< горизонт последней попытки
    uint32_t last_slept_ms   = 0u;   ///< результат последней попытки
    uint32_t profile_calls   = 0u;   ///< вызовов set_profile
    uint32_t last_profile    = 0u;   ///< аргумент последнего set_profile

    /// Сбросить наблюдение, сохранив настройку «железа».
    void resetCounters() noexcept {
        idle_calls = deep_calls = last_request_ms = last_slept_ms = 0u;
        profile_calls = last_profile = 0u;
    }
};

/// Единственный экземпляр модели (пишет платформа, читает тест).
PowerModel& power_model() noexcept;

} // namespace embed::mock_platform
