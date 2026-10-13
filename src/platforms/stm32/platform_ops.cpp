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
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    __DMB();
    return primask;
}

void platform_critical_exit(uint32_t state) noexcept {
    __DMB();
    __set_PRIMASK(state);
}

// ───────────────────────────────── Ядра ─────────────────────────────────────
uint32_t platform_core_count() noexcept { return 1u; }
uint32_t platform_current_core() noexcept { return 0u; }

// ──────────────────────────── Приоритеты ядра ───────────────────────────────
// PendSV и SysTick должны иметь САМЫЙ НИЗКИЙ приоритет, иначе смена
// контекста может произойти внутри другого обработчика.
void platform_setup_kernel_priorities() noexcept {
    NVIC_SetPriority(PendSV_IRQn,  0xFFu);
    NVIC_SetPriority(SysTick_IRQn, 0xFFu);
}

// ──────────────────────────────── Контекст ──────────────────────────────────
void platform_trigger_context_switch() noexcept {
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    __DSB();
    __ISB();
}

// Сохранение контекста: аппаратная часть (R0-R3, R12, LR, PC, xPSR) уже
// лежит в стеке задачи (PSP). Сохраняем R4-R11 и возвращаем новый SP.
//
// ВНИМАНИЕ: это обычная (не naked) функция. Прежний вариант был
// __attribute__((naked)) с extended asm и параметром — GCC этого не
// гарантирует. Здесь тело на basic asm, а значение возвращается через R0,
// что для AAPCS корректно.
__attribute__((naked)) uint32_t* platform_save_context() noexcept {
    __asm volatile(
        "   mrs r0, psp          \n"
        "   subs r0, r0, #32     \n"
        "   stmia r0!, {r4-r11}  \n"
        "   subs r0, r0, #32     \n"
        "   bx lr                \n"
    );
}

// Загрузка контекста: последний шаг PendSV. Выполняет аппаратный возврат
// из прерывания, намеренно не возвращаясь в C++.
__attribute__((naked)) void platform_load_context(uint32_t* sp) noexcept {
    (void)sp;
    __asm volatile(
        "   ldmia r0!, {r4-r11}  \n"
        "   msr psp, r0          \n"
        "   mov r0, #0           \n"
        "   msr basepri, r0      \n"
        "   mov r0, #0xFFFFFFFD  \n"
        "   bx r0                 \n"
    );
}

// Запуск первой задачи из main(): имитируем возврат из прерывания.
__attribute__((naked)) void platform_start_first_task(uint32_t* sp) noexcept {
    (void)sp;
    __asm volatile(
        "   msr psp, r0          \n"
        "   movs r0, #2          \n"
        "   msr control, r0      \n"
        "   isb                  \n"
        "   cpsie i              \n"
        "   mov r0, #0xFFFFFFFD  \n"
        "   bx r0                 \n"
    );
}

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
    if (!stack || words == 0u) return;
    // Дно — магическое значение для детекции переполнения.
    stack[0] = 0xDEADBEEFu;
    for (uint32_t i = 1u; i < words; ++i) stack[i] = 0xA5A5A5A5u;
}

// Стартовый фрейм задачи: аппаратная часть + программная (R4-R11).
static void task_exit_handler() noexcept {
    // Задача не должна возвращаться. Уходим в бесконечный цикл, чтобы
    // отладчик показал точное место, а не свалился в HardFault.
    for (;;) __asm volatile("nop");
}

uint32_t* platform_stack_init_frame(uint32_t* sp, void (*fn)(void*), void* ctx) noexcept {
    if (!sp || !fn) return nullptr;

    // Требование ARM EABI: SP выровнен по 8 байт на входе в функцию.
    sp = reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(sp) & ~0x07UL);

    // 1. Аппаратный фрейм (снимается ядром аппаратно при EXC_RETURN).
    *--sp = 0x01000000UL;                                     // xPSR, бит Thumb
    *--sp = static_cast<uint32_t>(static_cast<unsigned long>(reinterpret_cast<uintptr_t>(fn)));       // PC
    *--sp = static_cast<uint32_t>(static_cast<unsigned long>(reinterpret_cast<uintptr_t>(&task_exit_handler))); // LR
    *--sp = 0x12121212UL;                                     // R12
    *--sp = 0x03030303UL;                                     // R3
    *--sp = 0x02020202UL;                                     // R2
    *--sp = 0x01010101UL;                                     // R1
    *--sp = static_cast<uint32_t>(static_cast<unsigned long>(reinterpret_cast<uintptr_t>(ctx)));      // R0

    // 2. Программный фрейм (сохраняется вручную в platform_save_context).
    *--sp = 0x11111111UL;   // R11
    *--sp = 0x10101010UL;   // R10
    *--sp = 0x09090909UL;   // R9
    *--sp = 0x08080808UL;   // R8
    *--sp = 0x07070707UL;   // R7
    *--sp = 0x06060606UL;   // R6
    *--sp = 0x05050505UL;   // R5
    *--sp = 0x04040404UL;   // R4

    return sp;
}

// ─────────────────────────────── Простой ────────────────────────────────────
// WFI экономит ток; прерывание (SysTick) разбудит ядро.
void platform_idle() noexcept {
    __DSB();
    __WFI();
}

// ─────────────────────────── Привязка АЦП ──────────────────────────────────
// Драйвер регистрирует свой обработчик, а ADC1_2_IRQHandler его вызывает.
// Так платформа не зависит от объекта драйвера.
static void platform_adc_bind(void (*on_irq)() noexcept) OS_PLATFORM_FN {
    g_adc_irq_hook = on_irq;
}

// ───────────────────────────────── Лог ──────────────────────────────────────
// Вывод в ITM (SWO): не требует UART и не тянет newlib-форматтер.
// Если отладчик не подключён, запись просто игнорируется ядром.
static void platform_log(const char* str) OS_PLATFORM_FN {
    if (!str) return;
    while (*str != '\0') {
        if (*str == '\n') {
            while (ITM->PORT[0].u32 == 0u) { }
            ITM->PORT[0].u8 = static_cast<uint8_t>('\r');
        }
        while (ITM->PORT[0].u32 == 0u) { }
        ITM->PORT[0].u8 = static_cast<uint8_t>(*str++);
    }
    while (ITM->PORT[0].u32 == 0u) { }
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
    /* log                   */ &platform_log,
};

const Ops& ops() noexcept { return kOps; }

bool validate() noexcept { return true; }

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
