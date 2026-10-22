// arch/armv7m/frame.hpp
// ---------------------------------------------------------------------------
// Построение стартового фрейма задачи. Вынесено из context.cpp, чтобы
// раскладку можно было проверить host-тестом без ARM-ассемблера.
//
// FPU-фрейм (bool fpu=true):
//   [0..7]   R4..R11
//   [8..23]  S16..S31
//   [24..39] S0..S15
//   [40]     FPSCR
//   [41]     reserved
//   [42..49] R0,R1,R2,R3,R12,LR,PC,xPSR
// Итого 50 слов (200 Б).
//
// Без FPU:
//   [0..7] R4..R11, [8] R0, [9..11] R1..R3, [12] R12, [13] LR,
//   [14] PC, [15] xPSR. Итого 16 слов.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace embed::arch::armv7m {

inline void task_exit_loop() noexcept {
    for (;;) __asm volatile("nop");
}

inline uint32_t* build_task_frame(uint32_t* sp, void (*fn)(void*), void* ctx,
                                  bool fpu) noexcept {
    if (sp == nullptr || fn == nullptr) return nullptr;
    sp = reinterpret_cast<uint32_t*>(
        reinterpret_cast<uintptr_t>(sp) & ~static_cast<uintptr_t>(0x07u));

    if (fpu) {
        *--sp = 0x01000000u;                                    // xPSR
        *--sp = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn));
        *--sp = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&task_exit_loop));
        *--sp = 0x12121212u;                                    // R12
        *--sp = 0x03030303u;
        *--sp = 0x02020202u;
        *--sp = 0x01010101u;
        *--sp = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx));
        *--sp = 0u;                                             // reserved
        *--sp = 0u;                                             // FPSCR
        for (uint32_t i = 0u; i < 16u; ++i) *--sp = 0u;         // S15..S0
        for (uint32_t i = 0u; i < 16u; ++i) *--sp = 0u;         // S31..S16
        *--sp = 0x11111111u;
        *--sp = 0x10101010u;
        *--sp = 0x09090909u;
        *--sp = 0x08080808u;
        *--sp = 0x07070707u;
        *--sp = 0x06060606u;
        *--sp = 0x05050505u;
        *--sp = 0x04040404u;
    } else {
        *--sp = 0x01000000u;
        *--sp = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn));
        *--sp = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&task_exit_loop));
        *--sp = 0x12121212u;
        *--sp = 0x03030303u;
        *--sp = 0x02020202u;
        *--sp = 0x01010101u;
        *--sp = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx));
        *--sp = 0x11111111u;
        *--sp = 0x10101010u;
        *--sp = 0x09090909u;
        *--sp = 0x08080808u;
        *--sp = 0x07070707u;
        *--sp = 0x06060606u;
        *--sp = 0x05050505u;
        *--sp = 0x04040404u;
    }
    return sp;
}

} // namespace embed::arch::armv7m
