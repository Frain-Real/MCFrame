// platforms/stm32/platform_ops.cpp
// ---------------------------------------------------------------------------
// Платформенный контракт (embed::platform::ops()) для STM32F103
// (ARM Cortex-M3, medium density: 128 КБ Flash / 20 КБ SRAM).
//
// ВАЖНО: этот файл не содержит регистрового кода периферии — только
// критические секции, контекст задач, тик и распределение памяти.
// DMA/ADC остались в platform.cpp.
// ---------------------------------------------------------------------------
#include "../../os/iplatform.hpp"
#include "../../os/config/time_config.hpp"   // OsTimer для SysTick_Handler
#include "../../os/core/task/sheduler.hpp"   // тело обработчика переключения (PendSV)
#include "power_stm32.hpp"                   // энергосбережение (Sleep/STOP/профили)
#include "watchdog_stm32.hpp"                // сторожевой таймер (IWDG), причина сброса
#include "boot_stm32.hpp"                    // переход к образу прошивки (загрузчик)
#include "flash_stm32.hpp"                   // встроенная Flash: слот OTA и записи
#include "../../os/fault/fault.hpp"         // запись об аварии и отчёт
#include "../../arch/armv7m/arch.hpp"       // общий контекст ARMv7-M

#include "stm32f10x.h"
#include "core_cm3.h"

#include <cstdint>

// ─────────────── Регионы памяти: приходят из линкер-скрипта ────────────────
// Ядро больше НЕ знает про 0x20000000: адреса задаёт линкер, поэтому
// наложение кучи на .data/.bss/MSP становится ошибкой линковки, а не
// тихой порчей памяти в рантайме.
extern "C" {
    extern uint8_t  _os_heap_start[];
    extern uint8_t  _os_heap_end[];
    extern uint32_t _os_stack_pool_start[];
    extern uint32_t _os_stack_pool_end[];
    extern uint32_t _estack[];
}

namespace embed {

/// Хук обработчика АЦП. Живёт вне platform, чтобы его видел
/// ADC1_2_IRQHandler, объявленный на уровне embed.
void (*g_adc_irq_hook)() noexcept = nullptr;

/// Хуки обработчиков I2C (см. platform::Ops::i2c_bind): обработчик в таблице
/// векторов зовёт хук, а не драйвер, — так образ, которому I2C не нужен, не
/// тащит его автомат.
void (*g_i2c_ev_hook)() noexcept = nullptr;
void (*g_i2c_er_hook)() noexcept = nullptr;

namespace platform {

namespace {

constexpr uint32_t kTickClockHz   = 1000u;      // SysTick считает миллисекунды
constexpr uint32_t kSysTickMaxReload = 0x00FFFFFFu;

// Тик по умолчанию до первого platform_timer_set_period_ms().
uint32_t g_tick_ms = 1u;

} // namespace

// ───────────────────────────── Критическая секция ────────────────────────────
// Возвращаем ПРЕДЫДУЩИЙ PRIMASK, а не void: только так вложенные Lock
// не включают прерывания раньше внешней секции.
uint32_t platform_critical_enter() noexcept {
    return embed::arch::armv7m::critical_enter();
}

void platform_critical_exit(uint32_t state) noexcept {
    embed::arch::armv7m::critical_exit(state);
}

// ───────────────────────────────── Ядра ────────────────────────────────────
// ───────────────────────────────── Ядра ─────────────────────────────────────
uint32_t platform_core_count() noexcept { return 1u; }
uint32_t platform_current_core() noexcept { return 0u; }

// ──────────────────────────── Приоритеты ядра ───────────────────────────────
void platform_setup_kernel_priorities() noexcept {
    embed::arch::armv7m::setup_kernel_priorities();
}

// ──────────────────────────────── Контекст ──────────────────────────────────
// Общий ARMv7-M порт (arch/armv7m): PendSV/SVC и переключение задач.
void platform_trigger_context_switch() noexcept {
    embed::arch::armv7m::trigger_context_switch();
}

uint32_t* platform_save_context() noexcept {
    return embed::arch::armv7m::save_context();
}

void platform_load_context(uint32_t* sp) noexcept {
    embed::arch::armv7m::load_context(sp);
}

void platform_start_first_task(uint32_t* sp) noexcept {
    embed::arch::armv7m::start_first_task(sp);
}

// ───────────────────────────────── Время ────────────────────────────────────
// ───────────────────────────────── Время ────────────────────────────────────
void platform_timer_init() noexcept {
    platform_setup_kernel_priorities();
    SysTick->CTRL = 0u;
    SysTick->LOAD = (SystemCoreClock / kTickClockHz) * g_tick_ms - 1u;
    SysTick->VAL  = 0u;
}

bool platform_timer_can_period_ms(uint32_t period_ms) noexcept {
    if (period_ms == 0u) return false;
    const uint32_t reload = (SystemCoreClock / kTickClockHz) * period_ms;
    if (reload == 0u) return false;
    return reload <= (kSysTickMaxReload + 1u);
}

void platform_timer_set_period_ms(uint32_t period_ms) noexcept {
    g_tick_ms = period_ms;
    SysTick->LOAD = (SystemCoreClock / kTickClockHz) * period_ms - 1u;
    SysTick->VAL  = 0u;
}

void platform_timer_start() noexcept {
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |
                    SysTick_CTRL_TICKINT_Msk   |
                    SysTick_CTRL_ENABLE_Msk;
}

void platform_timer_stop() noexcept {
    SysTick->CTRL = 0u;
}

// ───────────────────────────────── Память ───────────────────────────────────
uint8_t* platform_heap_base() noexcept { return _os_heap_start; }

Size platform_heap_capacity() noexcept {
    return static_cast<Size>(_os_heap_end - _os_heap_start);
}

uint32_t* platform_stack_pool_base() noexcept { return _os_stack_pool_start; }

Size platform_stack_pool_capacity() noexcept {
    return static_cast<Size>(_os_stack_pool_end - _os_stack_pool_start);  // в словах
}

// Внешняя память: у STM32F103RB нет FSMC.
bool platform_has_ext_ram() noexcept { return false; }
uint32_t* platform_ext_ram_base() noexcept { return nullptr; }
Size platform_ext_ram_capacity() noexcept { return 0u; }

bool platform_has_ext_rom() noexcept { return false; }
Size platform_ext_rom_capacity() noexcept { return 0u; }
bool platform_ext_rom_read(uint32_t, uint8_t*, uint32_t) noexcept { return false; }
bool platform_ext_rom_write(uint32_t, const uint8_t*, uint32_t) noexcept { return false; }
bool platform_ext_rom_erase(uint32_t, uint32_t) noexcept { return false; }

// ─────────────────────────────── Стек задачи ────────────────────────────────
void platform_stack_paint(uint32_t* stack, uint32_t words) noexcept {
    embed::arch::armv7m::stack_paint(stack, words);
}

uint32_t* platform_stack_init_frame(uint32_t* sp, void (*fn)(void*), void* ctx) noexcept {
    return embed::arch::armv7m::stack_init_frame(sp, fn, ctx);
}

// ───────────────────────────── Аварийный сброс ──────────────────────────────
// NVIC_SystemReset() выставляет SYSRESETREQ и ждёт. Возврата не предполагается;
// если он всё же случится (отладчик/модель), fault::panic останется в цикле.
static void platform_reset() OS_PLATFORM_FN {
    NVIC_SystemReset();
}

static void platform_fault_enable() OS_PLATFORM_FN {
    embed::arch::armv7m::fpu_enable();
    // Без этих битов MemManage/BusFault/UsageFault уходят в HardFault.
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk |
                  SCB_SHCSR_BUSFAULTENA_Msk |
                  SCB_SHCSR_USGFAULTENA_Msk;
    __DSB();
    __ISB();
}

// ───────────────────────────── Fault-обработчики ────────────────────────────
// Определения ЖИВУТ ЗДЕСЬ, а не в отдельном fault_stm32.cpp: startup-файл
// содержит СЛАБЫЕ алиасы HardFault_Handler -> Default_Handler, и линкер не
// тянет сильное определение из архива только ради перекрытия слабого (оно
// уже «определено»). platform_ops.o линкуется всегда (нужен ops()), поэтому
// именно здесь сильные обработчики гарантированно попадут в образ и
// перекроют слабые.
// Общий C-обработчик. Определён ЗДЕСЬ, а не в отдельном объекте: иначе
// LTO/архив может не вытянуть его ради inline-asm ветки `b fault_c`.
extern "C" __attribute__((used)) void fault_c(uint32_t* frame,
                                              uint32_t exc_return,
                                              uint32_t kind) {
    (void)exc_return;   // стек уже выбран ассемблером

    embed::fault::Info info{};
    info.kind = static_cast<embed::fault::Kind>(kind + 1u);
    info.pc   = frame[6];
    info.lr   = frame[5];
    info.cfsr = SCB->CFSR;

#if !defined(MCFRAME_FAULT_REPORT) || (MCFRAME_FAULT_REPORT)
    // В тихом режиме эти поля не нужны отчёту — не тратим на них Flash/RAM.
    info.psr   = frame[7];
    info.r0    = frame[0];
    info.r1    = frame[1];
    info.r2    = frame[2];
    info.r3    = frame[3];
    info.r12   = frame[4];
    info.sp    = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(frame));
    info.hfsr  = SCB->HFSR;
    info.mmfar = SCB->MMFAR;
    info.bfar  = SCB->BFAR;
#endif

    embed::fault::capture(info);
    embed::fault::report();

    const embed::platform::Ops& ops = embed::platform::ops();
    if (ops.reset != nullptr) ops.reset();
    for (;;) {
        __asm volatile("" ::: "memory");
    }
}

#define MCFRAME_FAULT_WRAPPER(name, kind_value)                                \
    extern "C" void name(void) __attribute__((naked, used));                   \
    extern "C" void name(void) {                                               \
        __asm volatile(                                                        \
            "tst lr, #4          \n"                                          \
            "ite eq              \n"                                          \
            "mrseq r0, msp       \n"                                          \
            "mrsne r0, psp       \n"                                          \
            "mov r1, lr          \n"                                          \
            "movs r2, #" #kind_value " \n"                                    \
            "b fault_c           \n");                                        \
    }

MCFRAME_FAULT_WRAPPER(HardFault_Handler, 0)
MCFRAME_FAULT_WRAPPER(MemManage_Handler, 1)
MCFRAME_FAULT_WRAPPER(BusFault_Handler, 2)
MCFRAME_FAULT_WRAPPER(UsageFault_Handler, 3)

#undef MCFRAME_FAULT_WRAPPER

// ─────────────────────────────── Простой ────────────────────────────────────
// WFI экономит ток; прерывание (SysTick) разбудит ядро.
//
// Глубокий сон (STOP) здесь НЕ вызывается: Ops::idle — это «останови ядро»,
// безусловно безопасное действие. Уходить в STOP решает супервизор
// энергосбережения (os/power/power.hpp), который сначала проверяет источник
// пробуждения и сроки, — см. platform_power_* ниже.
void platform_idle() noexcept {
    __DSB();
    __WFI();
}

// ─────────────────────────── Энергосбережение ───────────────────────────────
// Таблица power — тонкая обёртка над embed::stm32::power (power_stm32.cpp):
// политика живёт в ядре (os/power), а регистры RTC/PWR/EXTI/FLASH — здесь.
namespace {

uint32_t platform_power_caps() OS_PLATFORM_FN {
    return embed::stm32::power::caps();
}

uint32_t platform_power_sysclk_hz() OS_PLATFORM_FN {
    return embed::stm32::power::sysclk_hz();
}

uint32_t platform_power_set_profile(uint32_t profile) OS_PLATFORM_FN {
    return embed::stm32::power::set_profile(profile);
}

uint32_t platform_power_deep_horizon_ms() OS_PLATFORM_FN {
    return embed::stm32::power::deep_sleep_horizon_ms();
}

uint32_t platform_power_deep_granularity_ms() OS_PLATFORM_FN {
    return embed::stm32::power::deep_sleep_granularity_ms();
}

uint32_t platform_power_deep_sleep(uint32_t max_ms) OS_PLATFORM_FN {
    return embed::stm32::power::deep_sleep(max_ms);
}

const PowerOps kPowerOps{
    /* caps                      */ &platform_power_caps,
    /* sysclk_hz                 */ &platform_power_sysclk_hz,
    /* set_profile               */ &platform_power_set_profile,
    /* deep_sleep_horizon_ms     */ &platform_power_deep_horizon_ms,
    /* deep_sleep_granularity_ms */ &platform_power_deep_granularity_ms,
    /* deep_sleep                */ &platform_power_deep_sleep,
};

// ─────────────────────────── Сторожевой таймер ──────────────────────────────
// Независимый таймер (IWDG) и причина последнего сброса; политика — в ядре
// (os/watchdog), регистры — в watchdog_stm32.cpp.
uint32_t platform_wd_caps() OS_PLATFORM_FN {
    return embed::stm32::watchdog::caps();
}

uint32_t platform_wd_closest_timeout_ms(uint32_t requested_ms) OS_PLATFORM_FN {
    return embed::stm32::watchdog::closest_timeout_ms(requested_ms);
}

bool platform_wd_start(uint32_t timeout_ms) OS_PLATFORM_FN {
    return embed::stm32::watchdog::start(timeout_ms);
}

void platform_wd_kick() OS_PLATFORM_FN {
    embed::stm32::watchdog::kick();
}

bool platform_wd_stop() OS_PLATFORM_FN {
    return embed::stm32::watchdog::stop();
}

bool platform_wd_note_write(uint32_t index, uint16_t value) OS_PLATFORM_FN {
    return embed::stm32::watchdog::note_write(index, value);
}

uint16_t platform_wd_note_read(uint32_t index) OS_PLATFORM_FN {
    return embed::stm32::watchdog::note_read(index);
}

uint32_t platform_wd_read_reset_cause() OS_PLATFORM_FN {
    return embed::stm32::watchdog::read_reset_cause();
}

const WatchdogOps kWatchdogOps{
    /* caps               */ &platform_wd_caps,
    /* closest_timeout_ms */ &platform_wd_closest_timeout_ms,
    /* start              */ &platform_wd_start,
    /* kick               */ &platform_wd_kick,
    /* stop               */ &platform_wd_stop,
    /* note_write         */ &platform_wd_note_write,
    /* note_read          */ &platform_wd_note_read,
    /* read_reset_cause   */ &platform_wd_read_reset_cause,
};

} // namespace

// ─────────────────────────── Привязка АЦП ──────────────────────────────────
// Драйвер регистрирует свой обработчик, а ADC1_2_IRQHandler его вызывает.
// Так платформа не зависит от объекта драйвера.
static void platform_adc_bind(void (*on_irq)() noexcept) OS_PLATFORM_FN {
    g_adc_irq_hook = on_irq;
}

// Привязка драйвера I2C: обработчики в таблице векторов зовут хуки, а не
// драйвер — поэтому образ, который I2C не использует, не содержит его автомат
// (см. platform.cpp и README, «Размер образа и что его определяет»).
static void platform_i2c_bind(void (*on_ev)() noexcept,
                              void (*on_er)() noexcept) OS_PLATFORM_FN {
    g_i2c_ev_hook = on_ev;
    g_i2c_er_hook = on_er;
}

// ───────────────────────────────── Лог ──────────────────────────────────────
// Вывод в ITM (SWO): не требует UART и не тянет newlib-форматтер.
// Если отладчик не подключён, запись просто игнорируется ядром.
/// Готов ли ITM-порт, но НЕ бесконечно: без подключённого отладчика
/// ITM->PORT[0].u32 не становится ненулевым, и прежний `while (...) {}`
/// вешал ядро прямо в логе. Для обычного лога это «просто нет вывода», а для
/// Fault-обработчика — несостоявшийся reset. Ждём ограниченно и бросаем
/// символ, а не задачу.
static bool itm_tx_ready() OS_PLATFORM_FN {
    constexpr uint32_t kSpinLimit = 10000u;
    for (uint32_t i = 0u; i < kSpinLimit; ++i) {
        if (ITM->PORT[0].u32 != 0u) return true;
    }
    return false;
}

static void platform_log(const char* str) OS_PLATFORM_FN {
    if (!str) return;
    while (*str != '\0') {
        if (*str == '\n') {
            if (!itm_tx_ready()) return;
            ITM->PORT[0].u8 = static_cast<uint8_t>('\r');
        }
        if (!itm_tx_ready()) return;
        ITM->PORT[0].u8 = static_cast<uint8_t>(*str++);
    }
    if (!itm_tx_ready()) return;
    ITM->PORT[0].u8 = static_cast<uint8_t>('\n');
}

// ─────────────────────────────── Таблица Ops ────────────────────────────────
static const Ops kOps{
    /* critical_enter        */ &platform_critical_enter,
    /* critical_exit         */ &platform_critical_exit,
    /* core_count            */ &platform_core_count,
    /* current_core          */ &platform_current_core,
    /* start_first_task      */ &platform_start_first_task,
    /* trigger_context_switch*/ &platform_trigger_context_switch,
    /* save_context          */ &platform_save_context,
    /* load_context          */ &platform_load_context,
    /* timer_init            */ &platform_timer_init,
    /* timer_can_period_ms   */ &platform_timer_can_period_ms,
    /* timer_set_period_ms   */ &platform_timer_set_period_ms,
    /* timer_start           */ &platform_timer_start,
    /* timer_stop            */ &platform_timer_stop,
    /* heap_base             */ &platform_heap_base,
    /* heap_capacity         */ &platform_heap_capacity,
    /* stack_pool_base       */ &platform_stack_pool_base,
    /* stack_pool_capacity   */ &platform_stack_pool_capacity,
    /* has_ext_ram           */ &platform_has_ext_ram,
    /* ext_ram_base          */ &platform_ext_ram_base,
    /* ext_ram_capacity      */ &platform_ext_ram_capacity,
    /* has_ext_rom           */ &platform_has_ext_rom,
    /* ext_rom_capacity      */ &platform_ext_rom_capacity,
    /* ext_rom_read          */ &platform_ext_rom_read,
    /* ext_rom_write         */ &platform_ext_rom_write,
    /* ext_rom_erase         */ &platform_ext_rom_erase,
    /* stack_paint           */ &platform_stack_paint,
    /* stack_init_frame      */ &platform_stack_init_frame,
    /* idle                  */ &platform_idle,
    /* adc_bind              */ &platform_adc_bind,
    /* i2c_bind              */ &platform_i2c_bind,
    /* log                   */ &platform_log,
    /* power                 */ &kPowerOps,
    /* watchdog              */ &kWatchdogOps,
    /* boot                  */ &embed::stm32::boot::ops(),
    /* flash                 */ &embed::stm32::flash::ops(),
    /* reset                 */ &platform_reset,
    /* fault_enable          */ &platform_fault_enable,
};

const Ops& ops() noexcept { return kOps; }

bool validate() noexcept {
    // Проверяем ровно то, что не может проверить компилятор. Отсутствие
    // таблицы энергосбережения проявилось бы разыменованием nullptr в
    // idle-задаче, то есть падением в простое — самом «тихом» месте системы.
    if (kOps.power == nullptr) return false;
    if (kOps.power->caps == nullptr) return false;
    if (kOps.power->sysclk_hz == nullptr) return false;
    if (kOps.power->set_profile == nullptr) return false;
    if (kOps.power->deep_sleep == nullptr) return false;
    if (kOps.power->deep_sleep_horizon_ms == nullptr) return false;
    if (kOps.power->deep_sleep_granularity_ms == nullptr) return false;

    // Сторожевой таймер: его отсутствие проявилось бы падением в прерывании
    // тика — то есть ровно там, где защита от зависаний и нужна.
    if (kOps.watchdog == nullptr) return false;
    if (kOps.watchdog->caps == nullptr) return false;
    if (kOps.watchdog->closest_timeout_ms == nullptr) return false;
    if (kOps.watchdog->start == nullptr) return false;
    if (kOps.watchdog->kick == nullptr) return false;
    if (kOps.watchdog->stop == nullptr) return false;

    // Переход к другой прошивке: его отсутствие проявилось бы отказом
    // загрузчика ровно в тот момент, когда исправлять уже нечего.
    if (kOps.i2c_bind == nullptr) return false;
    if (kOps.boot == nullptr) return false;
    if (kOps.boot->image_base == nullptr) return false;
    if (kOps.boot->image_plausible == nullptr) return false;
    if (kOps.boot->jump == nullptr) return false;
    if (kOps.boot->relocate_vectors == nullptr) return false;

    // Встроенная Flash: её отсутствие проявилось бы отказом записи посреди
    // обновления — то есть когда слот уже стёрт, а откатиться некуда.
    if (kOps.flash == nullptr) return false;
    if (kOps.flash->caps == nullptr) return false;
    if (kOps.flash->capacity == nullptr) return false;
    if (kOps.flash->page_size == nullptr) return false;
    if (kOps.flash->write_granularity == nullptr) return false;
    if (kOps.flash->unlock == nullptr) return false;
    if (kOps.flash->lock == nullptr) return false;
    if (kOps.flash->erase == nullptr) return false;
    if (kOps.flash->program == nullptr) return false;
    if (kOps.flash->read == nullptr) return false;

    // Аварийный сброс: без него panic()/Fault не смогли бы завершиться.
    if (kOps.reset == nullptr) return false;
    if (kOps.fault_enable == nullptr) return false;
    return true;
}

} // namespace platform
} // namespace embed

// ─────────────────────────── Обработчик тика ОС ─────────────────────────────
// Без него SysTick не вызывает TimerModule::TickIsr(), время стоит,
// NowMs() == 0 и все таймауты работают неверно.
extern "C" void SysTick_Handler(void) {
    embed::config::OsTimer::TickIsr();
}

// ADC1 и ADC2 делят одно прерывание на STM32F1.
extern "C" void ADC1_2_IRQHandler(void) {
    if (embed::g_adc_irq_hook != nullptr) {
        embed::g_adc_irq_hook();
    }
}

#if MCFRAME_STM32_DEEP_SLEEP
// Будильник RTC (линия EXTI17). Ядро выходит из STOP ДО этого обработчика:
// здесь только снимаются флаги, иначе прерывание «дребезжит» и мешает
// измерению времени сна.
extern "C" void RTCAlarm_IRQHandler(void) {
    embed::stm32::power::on_alarm_irq();
}
#endif
