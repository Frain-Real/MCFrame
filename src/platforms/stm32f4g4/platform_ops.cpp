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

uint32_t flash_caps() noexcept {
    return static_cast<uint32_t>(FlashCaps::Internal) |
           static_cast<uint32_t>(FlashCaps::Lockable) |
           static_cast<uint32_t>(FlashCaps::ErasePage);
}
uint32_t flash_base() noexcept { return kFlashBase; }
uint32_t flash_capacity() noexcept { return kFlashCapacity; }
uint32_t flash_page() noexcept {
#if defined(MCFRAME_SOC_STM32G4)
    return 2048u;
#else
    // F4: минимальный сектор — 16K (секторы 0-3). Остальные секторы 64/128K,
    // но 16K — корректная единица для областей, выровненных по секторам:
    // flash_erase() всё равно выбирает фактический сектор по адресу.
    return 16u * 1024u;
#endif
}
uint32_t flash_gran() noexcept {
#if defined(MCFRAME_SOC_STM32G4)
    return 8u;
#else
    return 1u;
#endif
}
bool flash_wait() noexcept {
    uint32_t guard = 2000000u;
    while ((FLASH->SR & FLASH_SR_BSY) != 0u) {
        if (--guard == 0u) return false;
    }
    FLASH->SR = FLASH_SR_EOP;
    return true;
}
bool flash_unlock() noexcept {
    if ((FLASH->CR & FLASH_CR_LOCK) == 0u) return true;
    FLASH->KEYR = FLASH_KEY1;
    FLASH->KEYR = FLASH_KEY2;
    return (FLASH->CR & FLASH_CR_LOCK) == 0u;
}
void flash_lock() noexcept { FLASH->CR |= FLASH_CR_LOCK; }
bool flash_erase(uint32_t addr, uint32_t len) noexcept {
    if (addr < kFlashBase || len == 0u) return false;
    if ((addr - kFlashBase) + len > kFlashCapacity) return false;
#if defined(MCFRAME_SOC_STM32G4)
    const uint32_t page = 2048u;
    uint32_t pos = addr;
    while (pos < addr + len) {
        FLASH->CR = FLASH_CR_PER | (((pos - kFlashBase) / page) << 3);
        FLASH->CR |= FLASH_CR_STRT;
        if (!flash_wait()) return false;
        FLASH->CR &= ~FLASH_CR_PER;
        pos += page;
    }
#else
    static const uint32_t kSectorOffsets[] = {
        0x00000u, 0x04000u, 0x08000u, 0x0C000u, 0x10000u, 0x20000u,
        0x40000u, 0x60000u, 0x80000u, 0xA0000u, 0xC0000u, 0xE0000u, 0x100000u
    };
    uint32_t end = addr + len;
    while (addr < end) {
        const uint32_t off = addr - kFlashBase;
        uint32_t sector = 0u;
        for (uint32_t i = 0u; i < 12u; ++i) {
            if (off >= kSectorOffsets[i]) sector = i;
        }
        FLASH->CR = FLASH_CR_SER | (sector << 3);
        FLASH->CR |= FLASH_CR_STRT;
        if (!flash_wait()) return false;
        FLASH->CR &= ~FLASH_CR_SER;
        addr = kFlashBase + kSectorOffsets[sector + 1u];
    }
#endif
    return true;
}
bool flash_program(uint32_t addr, const void* data, uint32_t len) noexcept {
    if (data == nullptr || addr < kFlashBase) return false;
    if ((addr - kFlashBase) + len > kFlashCapacity) return false;
#if defined(MCFRAME_SOC_STM32G4)
    const auto* src = static_cast<const uint8_t*>(data);
    uint32_t done = 0u;
    while (done < len) {
        uint64_t word = 0xFFFFFFFFFFFFFFFFull;
        for (uint32_t i = 0u; i < 8u && done + i < len; ++i) {
            word &= ~(0xFFull << (8u * i));
            word |= uint64_t(src[done + i]) << (8u * i);
        }
        FLASH->CR |= FLASH_CR_PG;
        *reinterpret_cast<volatile uint64_t*>(addr + done) = word;
        if (!flash_wait()) return false;
        FLASH->CR &= ~FLASH_CR_PG;
        done += 8u;
    }
#else
    const auto* src = static_cast<const uint8_t*>(data);
    FLASH->CR |= FLASH_CR_PG;
    for (uint32_t i = 0u; i < len; ++i) {
        *reinterpret_cast<volatile uint8_t*>(addr + i) = src[i];
        if (!flash_wait()) return false;
    }
    FLASH->CR &= ~FLASH_CR_PG;
#endif
    return true;
}
bool flash_read(uint32_t addr, void* data, uint32_t len) noexcept {
    if (data == nullptr || addr < kFlashBase) return false;
    const auto* src = reinterpret_cast<const uint8_t*>(addr);
    auto* dst = static_cast<uint8_t*>(data);
    for (uint32_t i = 0u; i < len; ++i) dst[i] = src[i];
    return true;
}
const FlashOps kFlashOps{&flash_caps, &flash_base, &flash_capacity,
                         &flash_page, &flash_gran, &flash_unlock, &flash_lock,
                         &flash_erase, &flash_program, &flash_read};

uint32_t boot_image_base() noexcept {
    return *reinterpret_cast<volatile uint32_t*>(0xE000ED08u);
}
bool boot_plausible(uint32_t addr, uint32_t len) noexcept {
    if (addr < kFlashBase || len < 8u) return false;
    if ((addr - kFlashBase) + len > kFlashCapacity) return false;
    const uint32_t sp = *reinterpret_cast<const volatile uint32_t*>(addr);
    const uint32_t rv = *reinterpret_cast<const volatile uint32_t*>(addr + 4u);
    const bool sp_ok = sp > 0x20000000u && sp <= 0x20000000u + 256u * 1024u;
    const bool rv_ok = (rv & 1u) != 0u && rv >= addr && rv < addr + len;
    return sp_ok && rv_ok;
}
bool boot_jump(uint32_t addr) noexcept {
    if (!boot_plausible(addr, kFlashCapacity - (addr - kFlashBase))) return false;
    const uint32_t sp = *reinterpret_cast<const volatile uint32_t*>(addr);
    const uint32_t rv = *reinterpret_cast<const volatile uint32_t*>(addr + 4u);
    __asm volatile ("cpsid i" ::: "memory");
    *reinterpret_cast<volatile uint32_t*>(0xE000ED08u) = addr;
    __asm volatile ("msr msp, %0" :: "r" (sp) : "memory");
    using Entry = void (*)();
    reinterpret_cast<Entry>(rv)();
    return true;
}
void boot_relocate(uint32_t addr) noexcept {
    *reinterpret_cast<volatile uint32_t*>(0xE000ED08u) = addr;
}
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
