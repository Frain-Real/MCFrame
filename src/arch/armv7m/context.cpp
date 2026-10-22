// arch/armv7m/context.cpp
// ---------------------------------------------------------------------------
// Контекст ARMv7-M. Для M4F (MCFRAME_ARCH_FPU=1) каждая задача имеет
// ФИКСИРОВАННЫЙ FPU-фрейм: S0–S15/FPSCR сохраняет аппаратура, S16–S31 и
// R4–R11 — программный код. Lazy stacking выключен, поэтому EXC_RETURN
// одинаков у всех задач и `bx lr` в PendSV корректен.
// ---------------------------------------------------------------------------
#include "arch.hpp"
#include "frame.hpp"

#include "../../os/core/task/sheduler.hpp"
#include "../../os/iplatform.hpp"

#include <cstdint>

#if MCFRAME_ARCH_FPU
  #define MCFRAME_FPU_ASM 1
#else
  #define MCFRAME_FPU_ASM 0
#endif

namespace {

// Регистры ядра, не требующие CMSIS-заголовков.
constexpr uintptr_t kScbBase   = 0xE000ED00u;
constexpr uintptr_t kScbIcsr   = kScbBase + 0x04u;
constexpr uintptr_t kScbShcsr  = kScbBase + 0x24u;
constexpr uintptr_t kScbCpacr  = kScbBase + 0x88u;
constexpr uintptr_t kFpuFpccr  = 0xE000EF34u;
constexpr uintptr_t kShpr3     = 0xE000ED20u;   // PendSV[23:16], SysTick[31:24]
constexpr uintptr_t kSysTick   = 0xE000E010u;

inline volatile uint32_t& reg32(uintptr_t addr) noexcept {
    return *reinterpret_cast<volatile uint32_t*>(addr);
}
inline volatile uint8_t& reg8(uintptr_t addr) noexcept {
    return *reinterpret_cast<volatile uint8_t*>(addr);
}

} // namespace

extern "C" {
__attribute__((used, externally_visible)) uint32_t* g_first_task_sp = nullptr;

// Вызывается из PendSV: выбор следующей задачи (C-код, возвращает SP).
__attribute__((used, externally_visible))
uint32_t* embed_pendsv_switch(uint32_t* saved_sp) noexcept {
    return embed::scheduler::instance().prepare_context_switch(saved_sp);
}

// Сохранение R4-R11 (+ FPU-часть) и возврат нового SP. Naked: не трогает MSP.
__attribute__((naked, used, externally_visible))
uint32_t* arch_save_context() noexcept {
#if MCFRAME_FPU_ASM
    __asm volatile(
        "   mrs r0, psp              \n"
        "   subs r0, r0, #96         \n"   // R4-R11 (32) + S16-S31 (64)
        "   stmia r0!, {r4-r11}      \n"
        "   vstmia r0!, {s16-s31}    \n"
        "   subs r0, r0, #96         \n"
        "   bx lr                    \n"
    );
#else
    __asm volatile(
        "   mrs r0, psp              \n"
        "   subs r0, r0, #32         \n"
        "   stmia r0!, {r4-r11}      \n"
        "   subs r0, r0, #32         \n"
        "   bx lr                    \n"
    );
#endif
}

__attribute__((naked, used, externally_visible))
void arch_load_context(uint32_t* sp) noexcept {
    (void)sp;
#if MCFRAME_FPU_ASM
    __asm volatile(
        "   ldmia r0!, {r4-r11}      \n"
        "   vldmia r0!, {s16-s31}    \n"
        "   msr psp, r0              \n"
        "   isb                      \n"
        "   mov r0, #0xFFFFFFED      \n"   // FPU-фрейм, PSP, thread
        "   bx r0                    \n"
    );
#else
    __asm volatile(
        "   ldmia r0!, {r4-r11}      \n"
        "   msr psp, r0              \n"
        "   mov r0, #0              \n"
        "   msr basepri, r0          \n"
        "   mov r0, #0xFFFFFFFD      \n"
        "   bx r0                     \n"
    );
#endif
}

// PendSV: сохранить уходящую, выбрать следующую, загрузить её.
__attribute__((naked)) void PendSV_Handler(void) {
    __asm volatile(
        "   bl   arch_save_context        \n"
        "   cpsid i                       \n"
        "   push {r4, lr}                 \n"
        "   bl   embed_pendsv_switch      \n"
        "   pop  {r4, lr}                 \n"
#if MCFRAME_FPU_ASM
        "   ldmia r0!, {r4-r11}           \n"
        "   vldmia r0!, {s16-s31}         \n"
        "   msr  psp, r0                  \n"
        "   isb                           \n"
        "   cpsie i                       \n"
        "   bx   lr                       \n"
#else
        "   ldmia r0!, {r4-r11}           \n"
        "   msr  psp, r0                  \n"
        "   isb                           \n"
        "   cpsie i                       \n"
        "   bx   lr                       \n"
#endif
    );
}

// Первый запуск: сохранить SP, обнулить MSP, войти в SVC.
__attribute__((naked, used, externally_visible))
void arch_start_first_task(uint32_t* sp) noexcept {
    (void)sp;
    __asm volatile(
        "   movw r1, #:lower16:g_first_task_sp   \n"
        "   movt r1, #:upper16:g_first_task_sp   \n"
        "   str r0, [r1]                          \n"
        "   movw r1, #:lower16:_estack            \n"
        "   movt r1, #:upper16:_estack            \n"
        "   msr msp, r1                           \n"
        "   cpsie i                               \n"
        "   dsb                                   \n"
        "   isb                                   \n"
#if MCFRAME_FPU_ASM
        "   mov r0, #4                            \n"   // CONTROL.FPCA
        "   msr control, r0                       \n"
        "   isb                                   \n"
#endif
        "   svc 0                                 \n"
        "   b .                                   \n"
    );
}

__attribute__((naked)) void SVC_Handler(void) {
    __asm volatile(
        "   movw r1, #:lower16:g_first_task_sp   \n"
        "   movt r1, #:upper16:g_first_task_sp   \n"
        "   ldr r0, [r1]                          \n"
        "   movs r2, #0                           \n"
        "   str r2, [r1]                          \n"
#if MCFRAME_FPU_ASM
        "   ldmia r0!, {r4-r11}                   \n"
        "   vldmia r0!, {s16-s31}                 \n"
        "   msr psp, r0                           \n"
        "   isb                                   \n"
        "   mov r0, #0xFFFFFFED                   \n"
        "   bx r0                                 \n"
#else
        "   ldmia r0!, {r4-r11}                   \n"
        "   msr psp, r0                           \n"
        "   isb                                   \n"
        "   mov r0, #0xFFFFFFFD                   \n"
        "   bx r0                                 \n"
#endif
    );
}
} // extern "C"

namespace embed::arch::armv7m {

uint32_t critical_enter() noexcept {
    uint32_t primask;
    __asm volatile("mrs %0, primask" : "=r"(primask));
    __asm volatile("cpsid i" ::: "memory");
    __asm volatile("dmb" ::: "memory");
    return primask;
}

void critical_exit(uint32_t state) noexcept {
    __asm volatile("dmb" ::: "memory");
    __asm volatile("msr primask, %0" :: "r"(state) : "memory");
}

void setup_kernel_priorities() noexcept {
    // SHPR3: PendSV — 0xFF, SysTick — 0xFF.
    reg8(kShpr3 + 2u) = 0xFFu;
    reg8(kShpr3 + 3u) = 0xFFu;
}

void trigger_context_switch() noexcept {
    reg32(kScbIcsr) = (1u << 28);   // PendSV set
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

uint32_t* save_context() noexcept { return arch_save_context(); }
void load_context(uint32_t* sp) noexcept { arch_load_context(sp); }
void start_first_task(uint32_t* sp) noexcept { arch_start_first_task(sp); }

void stack_paint(uint32_t* stack, uint32_t words) noexcept {
    if (stack == nullptr || words == 0u) return;
    stack[0] = embed::kStackGuardMagic;
    for (uint32_t i = 1u; i < words; ++i) stack[i] = 0xA5A5A5A5u;
}

uint32_t* stack_init_frame(uint32_t* sp, void (*fn)(void*), void* ctx) noexcept {
    return build_task_frame(sp, fn, ctx, MCFRAME_ARCH_FPU != 0);
}


void fpu_enable() noexcept {
#if MCFRAME_ARCH_FPU
    reg32(kScbCpacr) |= (0xFu << 20);       // CP10/CP11 full access
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
    // ASPEN=1, LSPEN=0: аппаратный FPU-фрейм создаётся сразу, раскладка
    // одинакова у всех задач.
    reg32(kFpuFpccr) = (reg32(kFpuFpccr) | (1u << 31)) & ~(1u << 30);
#else
    (void)kScbCpacr;
    (void)kFpuFpccr;
#endif
}

bool fpu_enabled() noexcept {
#if MCFRAME_ARCH_FPU
    return (reg32(kScbCpacr) & (0xFu << 20)) == (0xFu << 20);
#else
    return false;
#endif
}

} // namespace embed::arch::armv7m
