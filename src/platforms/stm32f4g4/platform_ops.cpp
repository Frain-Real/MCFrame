// platforms/stm32f4g4/platform_ops.cpp
// ---------------------------------------------------------------------------
// Phase 2: минимальный platform::Ops для F407/G431. Проверяется ядро:
// критические секции, контекст, FPU, SysTick и память. Периферия/драйверы
// (GPIO/SPI/PWM/UART) — Phase 3; power/watchdog/flash/boot пока заявляют
// caps=0 и не используются self-test-ом.
// ---------------------------------------------------------------------------
#include "../../os/iplatform.hpp"
#include "../../os/config/time_config.hpp"
#include "../../os/core/task/sheduler.hpp"
#include "../../arch/armv7m/arch.hpp"

#include "stm32m4_min.h"

#include <cstdint>

extern "C" {
extern uint8_t  _os_heap_start[];
extern uint8_t  _os_heap_end[];
extern uint32_t _os_stack_pool_start[];
extern uint32_t _os_stack_pool_end[];
extern uint32_t _estack[];
}

namespace embed::platform {
namespace {

constexpr uint32_t kTickClockHz = 1000u;
constexpr uint32_t kSysTickMaxReload = 0x00FFFFFFu;
uint32_t g_tick_ms = 1u;

#if defined(MCFRAME_SOC_STM32G4)
constexpr uint32_t kFlashBase = 0x08000000u;
constexpr uint32_t kFlashCapacity = 128u * 1024u;
#else
constexpr uint32_t kFlashBase = 0x08000000u;
constexpr uint32_t kFlashCapacity = 1024u * 1024u;
#endif

uint32_t platform_core_count() noexcept { return 1u; }
uint32_t platform_current_core() noexcept { return 0u; }

Size platform_heap_capacity() noexcept {
    return static_cast<Size>(_os_heap_end - _os_heap_start);
}
uint8_t* platform_heap_base() noexcept { return _os_heap_start; }
uint32_t* platform_stack_pool_base() noexcept { return _os_stack_pool_start; }
Size platform_stack_pool_capacity() noexcept {
    return static_cast<Size>(_os_stack_pool_end - _os_stack_pool_start);
}

bool platform_has_ext_ram() noexcept { return false; }
uint32_t* platform_ext_ram_base() noexcept { return nullptr; }
Size platform_ext_ram_capacity() noexcept { return 0u; }
bool platform_has_ext_rom() noexcept { return false; }
Size platform_ext_rom_capacity() noexcept { return 0u; }
bool platform_ext_rom_read(uint32_t, uint8_t*, uint32_t) noexcept { return false; }
bool platform_ext_rom_write(uint32_t, const uint8_t*, uint32_t) noexcept { return false; }
bool platform_ext_rom_erase(uint32_t, uint32_t) noexcept { return false; }

void platform_timer_init() noexcept {
    arch::armv7m::setup_kernel_priorities();
    SysTick->CTRL = 0u;
    SysTick->LOAD = (SystemCoreClock / kTickClockHz) * g_tick_ms - 1u;
    SysTick->VAL = 0u;
}
bool platform_timer_can_period_ms(uint32_t period_ms) noexcept {
    if (period_ms == 0u) return false;
    const uint32_t reload = (SystemCoreClock / kTickClockHz) * period_ms;
    return reload != 0u && reload <= (kSysTickMaxReload + 1u);
}
void platform_timer_set_period_ms(uint32_t period_ms) noexcept {
    g_tick_ms = period_ms;
    SysTick->LOAD = (SystemCoreClock / kTickClockHz) * period_ms - 1u;
    SysTick->VAL = 0u;
}
void platform_timer_start() noexcept {
    SysTick->CTRL = 7u;
}
void platform_timer_stop() noexcept {
    SysTick->CTRL = 0u;
}

void platform_idle() noexcept { __WFI(); }
void platform_reset() noexcept { NVIC_SystemReset(); }
void platform_fault_enable() noexcept { arch::armv7m::fpu_enable(); }
void platform_adc_bind(void (*)() noexcept) noexcept {}
void platform_i2c_bind(void (*)() noexcept, void (*)() noexcept) noexcept {}
void platform_log(const char*) noexcept {}

// ── Заглушки подсистем: caps=0, self-test их не использует ──
uint32_t power_caps() noexcept { return 0u; }
uint32_t power_sysclk() noexcept { return SystemCoreClock; }
uint32_t power_profile(uint32_t) noexcept { return 0u; }
uint32_t power_horizon() noexcept { return 0u; }
uint32_t power_granularity() noexcept { return 0u; }
uint32_t power_deep_sleep(uint32_t) noexcept { return 0u; }
const PowerOps kPowerOps{&power_caps, &power_sysclk, &power_profile,
                         &power_horizon, &power_granularity, &power_deep_sleep};

uint32_t wd_caps() noexcept { return 0u; }
uint32_t wd_closest(uint32_t) noexcept { return 0u; }
bool wd_start(uint32_t) noexcept { return false; }
void wd_kick() noexcept {}
bool wd_stop() noexcept { return false; }
bool wd_note_write(uint32_t, uint16_t) noexcept { return false; }
uint16_t wd_note_read(uint32_t) noexcept { return 0u; }
uint32_t wd_reset_cause() noexcept { return 0u; }
const WatchdogOps kWatchdogOps{&wd_caps, &wd_closest, &wd_start, &wd_kick,
                               &wd_stop, &wd_note_write, &wd_note_read,
                               &wd_reset_cause};

uint32_t flash_caps() noexcept { return 0u; }
uint32_t flash_base() noexcept { return kFlashBase; }
uint32_t flash_capacity() noexcept { return kFlashCapacity; }
uint32_t flash_page() noexcept { return 0u; }
uint32_t flash_gran() noexcept { return 1u; }
bool flash_unlock() noexcept { return false; }
void flash_lock() noexcept {}
bool flash_erase(uint32_t, uint32_t) noexcept { return false; }
bool flash_program(uint32_t, const void*, uint32_t) noexcept { return false; }
bool flash_read(uint32_t addr, void* data, uint32_t len) noexcept {
    if (data == nullptr) return false;
    const auto* src = reinterpret_cast<const uint8_t*>(addr);
    auto* dst = static_cast<uint8_t*>(data);
    for (uint32_t i = 0u; i < len; ++i) dst[i] = src[i];
    return true;
}
const FlashOps kFlashOps{&flash_caps, &flash_base, &flash_capacity,
                         &flash_page, &flash_gran, &flash_unlock, &flash_lock,
                         &flash_erase, &flash_program, &flash_read};

uint32_t boot_image_base() noexcept { return 0u; }
bool boot_plausible(uint32_t, uint32_t) noexcept { return false; }
bool boot_jump(uint32_t) noexcept { return false; }
void boot_relocate(uint32_t) noexcept {}
const BootOps kBootOps{&boot_image_base, &boot_plausible, &boot_jump,
                       &boot_relocate};

const Ops kOps{
    &arch::armv7m::critical_enter,
    &arch::armv7m::critical_exit,
    &platform_core_count,
    &platform_current_core,
    &arch::armv7m::start_first_task,
    &arch::armv7m::trigger_context_switch,
    &arch::armv7m::save_context,
    &arch::armv7m::load_context,
    &platform_timer_init,
    &platform_timer_can_period_ms,
    &platform_timer_set_period_ms,
    &platform_timer_start,
    &platform_timer_stop,
    &platform_heap_base,
    &platform_heap_capacity,
    &platform_stack_pool_base,
    &platform_stack_pool_capacity,
    &platform_has_ext_ram,
    &platform_ext_ram_base,
    &platform_ext_ram_capacity,
    &platform_has_ext_rom,
    &platform_ext_rom_capacity,
    &platform_ext_rom_read,
    &platform_ext_rom_write,
    &platform_ext_rom_erase,
    &arch::armv7m::stack_paint,
    &arch::armv7m::stack_init_frame,
    &platform_idle,
    &platform_adc_bind,
    &platform_i2c_bind,
    &platform_log,
    &kPowerOps,
    &kWatchdogOps,
    &kBootOps,
    &kFlashOps,
    &platform_reset,
    &platform_fault_enable,
};

} // namespace

const Ops& ops() noexcept { return kOps; }
bool validate() noexcept { return true; }

} // namespace embed::platform

extern "C" void SysTick_Handler(void) {
    embed::config::OsTimer::TickIsr();
}
