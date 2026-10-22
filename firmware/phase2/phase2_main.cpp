// firmware/phase2/phase2_main.cpp
// ---------------------------------------------------------------------------
// Phase 2 self-test: планировщик + PendSV/SVC + FPU-контекст.
//
// Задачи A и B непрерывно считают float, засыпая на каждом шаге; check
// просыпается позже и сверяет результаты. Если FPU-контекст не сохраняется,
// g_a/g_b разойдутся с эталоном.
//
// Успех/провал сообщается через semihosting (QEMU -semihosting). На реальной
// плате этот же образ можно запустить под отладчиком; UART появится в Phase 3.
// ---------------------------------------------------------------------------
#include "arch/armv7m/arch.hpp"
#include "os/common.hpp"
#include "os/config/time_config.hpp"
#include "os/core/memory/memory_manager.hpp"
#include "os/core/task/sheduler.hpp"

#include <cstdint>

using namespace embed;

namespace {

volatile float g_a = 0.0f;
volatile float g_b = 0.0f;

task    g_task_a;
task    g_task_b;
task    g_task_check;
uint32_t g_stack_a[256];
uint32_t g_stack_b[256];
uint32_t g_stack_check[256];

void semihost_exit(bool ok) noexcept {
    register uint32_t r0 __asm__("r0") = 0x18u;                 // SYS_EXIT
    register uint32_t r1 __asm__("r1") = ok ? 0x20026u : 0x20023u;
    __asm__ volatile("bkpt 0xAB" : : "r"(r0), "r"(r1) : "memory");
    for (;;) __asm__ volatile("nop");
}

void task_a(void*) noexcept {
    scheduler& s = scheduler::instance();
    for (uint32_t i = 0u; i < 20u; ++i) {
        g_a = g_a * 1.5f + 1.0f;
        (void)s.sleep_for(1u);
    }
    for (;;) (void)s.sleep_for(1000u);
}

void task_b(void*) noexcept {
    scheduler& s = scheduler::instance();
    for (uint32_t i = 0u; i < 20u; ++i) {
        g_b = g_b * 0.5f + 3.0f;
        (void)s.sleep_for(1u);
    }
    for (;;) (void)s.sleep_for(1000u);
}

void task_check(void*) noexcept {
    scheduler& s = scheduler::instance();
    (void)s.sleep_for(100u);

    float ea = 0.0f;
    float eb = 0.0f;
    for (uint32_t i = 0u; i < 20u; ++i) {
        ea = ea * 1.5f + 1.0f;
        eb = eb * 0.5f + 3.0f;
    }

    const float da = (g_a > ea) ? (g_a - ea) : (ea - g_a);
    const float db = (g_b > eb) ? (g_b - eb) : (eb - g_b);
    semihost_exit(da < 0.001f && db < 0.001f);
}

} // namespace

int main() {
    // FPU до первой float-инструкции и до задач.
    arch::armv7m::fpu_enable();

    if (g_memory_manager.init(2048u, 256u) != Status::Ok) return 1;

    scheduler& s = scheduler::instance();
    if (s.init() != Status::Ok) return 1;

    if (g_task_a.init(&task_a, nullptr, "fp-a", 256u, 1u) != Status::Ok) return 1;
    if (g_task_b.init(&task_b, nullptr, "fp-b", 256u, 1u) != Status::Ok) return 1;
    if (g_task_check.init(&task_check, nullptr, "check", 256u, 0u) != Status::Ok) return 1;

    if (!s.add_task(&g_task_a, g_stack_a, 256u)) return 1;
    if (!s.add_task(&g_task_b, g_stack_b, 256u)) return 1;
    if (!s.add_task(&g_task_check, g_stack_check, 256u)) return 1;

    config::OsTimer::Config tcfg{};
    if (config::OsTimer::Init(tcfg) != Status::Ok) return 1;
    if (config::OsTimer::Start() != Status::Ok) return 1;

    // Не возвращается: первая задача уходит через SVC/PendSV.
    (void)s.start();
    return 1;
}
