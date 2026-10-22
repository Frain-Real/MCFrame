// os_config.hpp
#pragma once

// ---------------------------------------------------------------------------
// Ёмкости ядра, влияющие на статическое потребление RAM.
//
// ЗАЧЕМ: прежние значения были выбраны «с запасом» и на одноядерном
// STM32F103 обходились почти в 2 КБ BSS — больше, чем все задачи примера.
// Здесь они собраны в одном месте и переопределяются из CMake
// (см. cmake/mcu_flags.cmake: MCFRAME_MAX_CORES / MCFRAME_MAX_PRIORITIES).
//
// Переопределение:
//   cmake .. -DMCFRAME_MAX_CORES=1 -DMCFRAME_MAX_PRIORITIES=4
// или таргетно:
//   target_compile_definitions(my_app PRIVATE MCFRAME_MAX_CORES=1)
// ---------------------------------------------------------------------------

/// Максимум ядер, под которые статически выделены очереди готовых задач.
///
/// Планировщик всё равно берёт фактическое число ядер у платформы
/// (platform::ops().core_count()), но массивы должны иметь размер,
/// известный на этапе компиляции. Для одного ядра ставьте 1.
#ifndef MCFRAME_MAX_CORES
#define MCFRAME_MAX_CORES 1u
#endif

/// Количество уровней приоритета (0 — высший).
/// Меньше уровней — меньше очередь готовых задач в каждом ядре.
#ifndef MCFRAME_MAX_PRIORITIES
#define MCFRAME_MAX_PRIORITIES 8u
#endif

/// Квант round-robin для задач ОДНОГО приоритета, тиков. 0 — выключено.
///
/// По умолчанию выключено: планировщик остаётся строго приоритетным, и
/// поведение не меняется от сборки к сборке. Включайте, если приложение
/// состоит из равноправных задач, которые не блокируются сами.
#ifndef MCFRAME_TIME_SLICE_TICKS
#define MCFRAME_TIME_SLICE_TICKS 0u
#endif

/// Тестовый хук планировщика (только mock): позволяет сигналить ровно в
/// момент, когда задача уже стоит в очереди ожидания, но ещё не уснула.
/// В прошивку не попадает.
#ifndef MCFRAME_TEST_HOOKS
#define MCFRAME_TEST_HOOKS 0
#endif

namespace embed::config {

    /// Максимальное число ядер (compile-time).
    inline constexpr unsigned kMaxCores = MCFRAME_MAX_CORES;

    /// Максимальное число приоритетов (compile-time).
    inline constexpr unsigned kMaxPriority = MCFRAME_MAX_PRIORITIES;

    /// Квант round-robin (0 = выключен).
    inline constexpr uint32_t kTimeSliceTicks = MCFRAME_TIME_SLICE_TICKS;

    static_assert(kMaxCores >= 1u, "MCFRAME_MAX_CORES должен быть >= 1");
    static_assert(kMaxCores <= 16u, "MCFRAME_MAX_CORES разумно ограничить 16");
    static_assert(kMaxPriority >= 2u, "нужно минимум 2 уровня приоритета");
    static_assert(kMaxPriority <= 32u, "маска приоритетов 32-битная");

} // namespace embed::config
