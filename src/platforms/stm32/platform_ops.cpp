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
// Имя — extern "C": эту функцию вызывает из ассемблера обработчик PendSV, и
// ему нужен СТАБИЛЬНЫЙ символ (mangled-имя C++ меняется от перегрузок и может
// быть переименовано LTO).
//
// ВНИМАНИЕ: это naked-функция (basic asm и возврат значения через R0 по
// AAPCS), поэтому её нельзя инлайнить и у неё нет пролога: стек она не трогает
// вовсе — это важно для обработчика, который обязан быть сбалансирован по MSP.
// used + externally_visible: на символ ссылается ТОЛЬКО встроенный asm
// (обработчик PendSV), а его компилятор не видит — без этих атрибутов LTO
// считает функцию/переменную неиспользуемой, переименовывает её во внутренний
// символ, и ассемблерная ссылка остаётся «undefined reference» (проверено).
extern "C" __attribute__((naked, used, externally_visible))
uint32_t* platform_save_context() noexcept {
    __asm volatile(
        "   mrs r0, psp          \n"
        "   subs r0, r0, #32     \n"
        "   stmia r0!, {r4-r11}  \n"
        "   subs r0, r0, #32     \n"
        "   bx lr                \n"
    );
}

// Загрузка контекста + возврат из прерывания.
//
// ОСТОРОЖНО: этот путь выполняет возврат из исключения ИЗНУТРИ C-вызова
// (`bx 0xFFFFFFFD`), поэтому кадры стека обработчика, из которого его позвали,
// остаются в MSP навсегда — на каждом переключении MSP утекал бы на десятки
// байт. Обработчик переключения задач (PendSV_Handler ниже) поэтому НЕ
// пользуется этой функцией, а делает загрузку и выход сам, сохраняя MSP.
// Функция оставлена как часть контракта (platform::Ops::load_context) для
// платформ, где переключение устроено иначе, и для хостовых тестов.
extern "C" __attribute__((naked)) void platform_load_context(uint32_t* sp) noexcept {
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

// ────────────────────── Смена контекста: обработчик PendSV ──────────────────
//
// ЕДИНСТВЕННОЕ место, где на МКУ меняется задача. Без него в таблице векторов
// стояла слабая заглушка из startup-файла (бесконечный цикл), то есть первый
// же yield()/вытеснение по тику вешало устройство.
//
// ПОЧЕМУ ОБРАБОТЧИК СБАЛАНСИРОВАН ПО MSP, А НЕ «УХОДИТ ИЗНУТРИ C»
// Переключение состоит из трёх шагов: сохранить R4-R11 уходящей задачи,
// выбрать следующую, загрузить её R4-R11 и вернуться из исключения. Если бы
// загрузку делал C-код с `bx 0xFFFFFFFD` (как platform_load_context), то кадры
// стека обработчика остались бы в MSP: исключение уже не вернётся, а MSP никто
// не размотает — за десятки переключений 1 КБ MSP кончились бы, и ядро
// затёрло бы .bss. Поэтому середина (выбор задачи) — обычный C-вызов, который
// ВОЗВРАЩАЕТ SP следующей задачи, а сохранение, загрузка и выход сделаны в
// asm: `push {r4, lr}` / `pop {r4, lr}` возвращают MSP ровно к тому состоянию,
// каким он был на входе в обработчик.
//
// ПОЧЕМУ ССЫЛКА НА EXC_RETURN ЖИВЁТ В LR
// На входе в исключение LR = EXC_RETURN (0xFFFFFFFD — «потоковый режим, PSP»),
// и именно он говорит процессору, откуда снимать аппаратный кадр. Мы его
// сохраняем в push и восстанавливаем в pop, а `bx lr` в конце — штатный выход
// из обработчика: аппаратный кадр снимается уже со стека НОВОЙ задачи.
//
// ПОЧЕМУ ПРЕРЫВАНИЯ ЗАПРЕЩЕНЫ НА ВРЕМЯ ВЫБОРА
// Обработчик меняет очереди готовых задач, а прерывание периферии (у него
// приоритет выше, чем у PendSV) может в этот момент разбудить задачу и полезть
// в те же очереди. Поэтому середина окружена cpsid/cpsie. На входе в PendSV
// PRIMASK=0 (с запрещёнными прерываниями исключение просто не начнётся),
// поэтому возврат в ноль корректен; оставить PRIMASK=1 нельзя — вернувшаяся
// задача осталась бы без прерываний навсегда.
extern "C" __attribute__((used, externally_visible))
uint32_t* embed_pendsv_switch(uint32_t* saved_sp) noexcept {
    return embed::scheduler::instance().prepare_context_switch(saved_sp);
}

extern "C" __attribute__((naked)) void PendSV_Handler(void) {
    __asm volatile(
        "   bl   platform_save_context        \n"  // r0 = SP сохранённого контекста
        "   cpsid i                           \n"  // критическая секция выбора
        "   push {r4, lr}                     \n"  // + выравнивание MSP по 8 (AAPCS)
        "   bl   embed_pendsv_switch          \n"  // r0 = SP следующей задачи
        "   pop  {r4, lr}                     \n"  // MSP как на входе в обработчик
        "   ldmia r0!, {r4-r11}               \n"
        "   msr  psp, r0                      \n"
        "   isb                               \n"
        "   cpsie i                           \n"  // PRIMASK возвращаем в 0
        "   bx   lr                           \n"  // EXC_RETURN: выход в новую задачу
    );
}

// ─────────────── Запуск первой задачи (первое переключение) ─────────────────
//
// Первое переключение НЕЛЬЗЯ делать через PendSV: уходящего контекста нет —
// ядро работает в потоковом режиме на MSP, а PSP не инициализирован, поэтому
// `bl platform_save_context` записал бы R4-R11 по мусорному адресу.
//
// Запуск делается в два шага, как в классических портах Cortex-M:
//   1) здесь: запомнить SP первой задачи, обнулить MSP (стек main больше не
//      нужен — задача живёт на PSP), разрешить прерывания и войти в обработчик
//      SVC (в потоковом режиме войти в режим обработчика иначе нечем);
//   2) SVC_Handler: загрузить контекст задачи и выйти из исключения с
//      EXC_RETURN = «потоковый режим, PSP» — задача начинает исполняться.
//
// Прежний вариант дёргал CONTROL.SPSEL прямо в потоковом режиме и заканчивал
// `bx 0xFFFFFFFD`: в потоковом режиме это не возврат из исключения, а переход
// по адресу 0xFFFFFFFC, то есть UsageFault — запуск первой задачи не работал.
// used + externally_visible по той же причине, что и выше: адрес этой
// переменной берётся только из asm (movw/movt) в обработчиках ниже.
extern "C" {
__attribute__((used, externally_visible)) uint32_t* g_first_task_sp = nullptr;
}

extern "C" __attribute__((naked)) void platform_start_first_task(uint32_t* sp) noexcept {
    (void)sp;
    __asm volatile(
        // Адреса грузим movw/movt, а НЕ `ldr =symbol`: `ldr =` требует
        // литерального пула, а naked-функция — это чистый asm без `.pool`,
        // поэтому пул уезжает далеко и ассемблер падает «offset out of range».
        // С LTO это вылезает сразу (проверено), без LTO — как повезёт с
        // раскладкой секции.
        "   movw r1, #:lower16:g_first_task_sp   \n"
        "   movt r1, #:upper16:g_first_task_sp   \n"
        "   str r0, [r1]                          \n"   // SP первой задачи
        "   movw r1, #:lower16:_estack            \n"
        "   movt r1, #:upper16:_estack            \n"
        "   msr msp, r1                           \n"   // чистый MSP: стек main не нужен
        "   cpsie i                               \n"   // прерывания разрешаем ДО задачи
        "   dsb                                   \n"
        "   isb                                   \n"
        "   svc 0                                 \n"   // → SVC_Handler
        "   b .                                   \n"   // недостижимо: управление у задачи
    );
}

// Продолжение запуска первой задачи: загрузить её контекст и вернуться из
// исключения в потоковый режим на PSP. Обработчик существует только для этого
// (SVC в проекте больше нигде не используется).
extern "C" __attribute__((naked)) void SVC_Handler(void) {
    __asm volatile(
        "   movw r1, #:lower16:g_first_task_sp   \n"   // без литерального пула
        "   movt r1, #:upper16:g_first_task_sp   \n"
        "   ldr r0, [r1]                          \n"
        "   movs r2, #0                           \n"
        "   str r2, [r1]                          \n"   // одноразово: второй запуск невозможен
        "   ldmia r0!, {r4-r11}                   \n"
        "   msr psp, r0                           \n"
        "   isb                                   \n"
        "   mov r0, #0xFFFFFFFD                   \n"   // потоковый режим, стек PSP
        "   bx r0                                 \n"
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
    /* power                 */ &kPowerOps,
    /* watchdog              */ &kWatchdogOps,
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
