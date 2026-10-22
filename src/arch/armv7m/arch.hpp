// arch/armv7m/arch.hpp
// ---------------------------------------------------------------------------
// Общий порт ARMv7-M (Cortex-M3/M4/M4F): критические секции, контекст,
// PendSV/SVC, запуск первой задачи и FPU-контекст.
//
// F103 (M3) и F407/G431 (M4F) используют один и тот же код; FPU-часть
// включается MCFRAME_ARCH_FPU.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

#ifndef MCFRAME_ARCH_FPU
#define MCFRAME_ARCH_FPU 0
#endif

namespace embed::arch::armv7m {

uint32_t critical_enter() noexcept;
void     critical_exit(uint32_t state) noexcept;

void setup_kernel_priorities() noexcept;
void trigger_context_switch() noexcept;

uint32_t* save_context() noexcept;
void      load_context(uint32_t* sp) noexcept;

void start_first_task(uint32_t* sp) noexcept;

void     stack_paint(uint32_t* stack, uint32_t words) noexcept;
uint32_t* stack_init_frame(uint32_t* sp, void (*fn)(void*), void* ctx) noexcept;

/// Включить CPACR/FPU и выбрать фиксированный FPU-фрейм (без lazy stacking),
/// чтобы у всех задач была одна и та же раскладка стека.
void fpu_enable() noexcept;
bool fpu_enabled() noexcept;

} // namespace embed::arch::armv7m
