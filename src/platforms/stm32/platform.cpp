// platform.cpp
// Единый платформозависимый слой для STM32F103RBTx (ARM Cortex-M3)
// Требования: CMSIS-заголовки (например, "stm32f10x.h" или "core_cm3.h") должны быть подключены в проекте.

#include <cstdint>
#include <cstddef>
#include "stm32f10x.h"
#include "core_cm3.h"
// Если CMSIS не подключен глобально, раскомментируйте нужное:
// #include "stm32f10x.h" 
// #include "core_cm3.h"

namespace embed {

// =============================================================================
// 1. КОНФИГУРАЦИЯ ПАМЯТИ (STM32F103RBTx: 20 КБ SRAM, 128 КБ Flash)
// =============================================================================
// Распределение 20 КБ (20480 байт) SRAM (адрес 0x20000000):
// - ~4 КБ: Основной стек (MSP) для прерываний и функции main()
// - 8 КБ: Куча (Heap) для динамических объектов ОС
// - 8 КБ: Пул стеков задач (2048 слов по 4 байта)
// =============================================================================

uint32_t platform_max_heap_size() noexcept {
    return 8U * 1024U; // 8192 байта
}

uint32_t platform_max_stack_pool_size() noexcept {
    return 2048U; // 2048 слов = 8192 байта
}

// STM32F103RBTx не имеет контроллера FSMC, внешняя память отсутствует
bool platform_has_ext_ram() noexcept { return false; }
uint32_t* platform_ext_ram_base() noexcept { return nullptr; }
uint32_t platform_max_ext_ram_size() noexcept { return 0U; }

bool platform_has_ext_rom() noexcept { return false; }
uint32_t platform_max_ext_rom_size() noexcept { return 0U; }
bool platform_ext_rom_read(uint32_t, uint8_t*, uint32_t) noexcept { return false; }
bool platform_ext_rom_write(uint32_t, const uint8_t*, uint32_t) noexcept { return false; }
bool platform_ext_rom_erase(uint32_t, uint32_t) noexcept { return false; }


// =============================================================================
// 2. УПРАВЛЕНИЕ СТЕКОМ ЗАДАЧ
// =============================================================================

// Точка входа при нештатном завершении задачи (если функция задачи сделала return)
static void task_exit_handler() noexcept {
    // В детерминированной ОС задача не должна возвращаться.
    // Зависаем или инициируем системный сброс.
    while (true) {
        __asm volatile ("nop");
    }
}

void platform_stack_paint(uint32_t* stack, uint32_t words) noexcept {
    if (words == 0U || stack == nullptr) return;
    
    // stack[0] — это младший адрес ("дно" стека). 
    // Оставляем магическое значение для детекции переполнения (проверка в memory_manager.cpp)
    stack[0] = 0xDEADBEEFU; 
    
    // Остальное заполняем паттерном для расчёта high-water mark
    for (uint32_t i = 1U; i < words; ++i) {
        stack[i] = 0xA5A5A5A5U;
    }
}

uint32_t* platform_stack_init_frame(uint32_t* sp, void (*fn)(void*), void* ctx) noexcept {
    if (sp == nullptr || fn == nullptr) return nullptr;

    // Выравнивание указателя стека по 8 байт (требование ARM EABI для Cortex-M)
    sp = reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(sp) & ~0x07UL);

    // 1. Аппаратный фрейм (сохраняется/восстанавливается ядром Cortex-M3 аппаратно)
    *--sp = 0x01000000UL;          // xPSR: бит Thumb (24) должен быть установлен
    *--sp = reinterpret_cast<uint32_t>(fn);       // PC: адрес функции задачи
    *--sp = reinterpret_cast<uint32_t>(task_exit_handler); // LR: адрес возврата при завершении
    *--sp = 0x12121212UL;          // R12
    *--sp = 0x03030303UL;          // R3
    *--sp = 0x02020202UL;          // R2
    *--sp = 0x01010101UL;          // R1
    *--sp = reinterpret_cast<uint32_t>(ctx);      // R0: аргумент функции (контекст)

    // 2. Программный фрейм (сохраняется/восстанавливается вручную в platform_save_context)
    *--sp = 0x11111111UL;          // R11
    *--sp = 0x10101010UL;          // R10
    *--sp = 0x09090909UL;          // R9
    *--sp = 0x08080808UL;          // R8
    *--sp = 0x07070707UL;          // R7
    *--sp = 0x06060606UL;          // R6
    *--sp = 0x05050505UL;          // R5
    *--sp = 0x04040404UL;          // R4

    return sp;
}


// =============================================================================
// 3. ПЛАНИРОВЩИК И ЯДРО (Cortex-M3 Specific)
// =============================================================================

uint32_t platform_core_count() noexcept {
    return 1; // STM32F103 — одноядерный МК
}

uint32_t platform_current_core() noexcept {
    return 0;
}

void platform_critical_enter() noexcept {
    // Глобальное отключение прерываний. 
    // Максимально просто, быстро и детерминированно для одноядерной системы.
    __disable_irq();
}

void platform_critical_exit() noexcept {
    __enable_irq();
}

void platform_trigger_context_switch() noexcept {
    // Запрашиваем выполнение PendSV (Pendable Service Call)
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    
    // Барьеры памяти гарантируют, что все изменения в планировщике 
    // записаны в RAM до начала выполнения обработчика прерывания
    __DSB();
    __ISB();
}

// Сохранение контекста текущей задачи.
// Вызывается из PendSV_Handler. Аппаратная часть (R0-R3, R12, LR, PC, xPSR) 
// уже сохранена ядром в стек (PSP). Сохраняем только R4-R11.
__attribute__((naked)) uint32_t* platform_save_context() noexcept {
    __asm volatile (
        "MRS R0, PSP       \n" // Читаем текущий указатель стека задачи (PSP)
        "SUB R0, R0, #32   \n" // Резервируем 32 байта (8 слов) под регистры R4-R11
        "STM R0, {R4-R11}  \n" // Сохраняем регистры общего назначения в стек
        "MSR PSP, R0       \n" // Обновляем PSP новым значением
        "BX LR             \n" // Возврат в C++-код (R0 содержит новый указатель стека)
        : : : "r0", "memory"
    );
}

// Загрузка контекста новой задачи.
// Эта функция является ПОСЛЕДНЕЙ в цепочке вызовов PendSV. 
// Она выполняет аппаратный возврат из прерывания (EXC_RETURN), 
// намеренно "забывая" вернуться в C++-код планировщика.
__attribute__((naked)) void platform_load_context(uint32_t* sp) noexcept {
    __asm volatile (
        // R0 содержит указатель на стек новой задачи
        "LDM R0!, {R4-R11} \n" // Восстанавливаем R4-R11. R0 теперь указывает на аппаратный фрейм
        "MSR PSP, R0       \n" // Устанавливаем PSP на начало аппаратного фрейма
        "MOV R0, #0        \n"
        "MSR BASEPRI, R0   \n" // Сбрасываем маску прерываний ОС (гарантируем, что прерывания разрешены)
        "MOV R0, #0xFFFFFFFD \n" // EXC_RETURN: возврат в Thread mode, использование PSP, без FPU
        "BX R0             \n" // Аппаратный возврат из прерывания (ядро восстановит R0-R3, R12, LR, PC, xPSR)
        : : : "r0", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "memory"
    );
}

// Запуск самой первой задачи из main().
// Имитирует возврат из прерывания, чтобы корректно развернуть аппаратный фрейм.
__attribute__((naked)) void platform_start_first_task(uint32_t* sp) noexcept {
    __asm volatile (
        "MSR PSP, R0       \n" // Устанавливаем PSP на стек первой задачи
        "MOV R0, #0x02     \n" // CONTROL = 0x02: Thread mode, привилегированный уровень, используется PSP
        "MSR CONTROL, R0   \n"
        "ISB               \n" // Барьер синхронизации команд (обязателен после записи в CONTROL)
        "CPSIE I           \n" // Глобально разрешаем прерывания перед стартом планировщика
        "MOV R0, #0xFFFFFFFD \n" // EXC_RETURN
        "BX R0             \n" // Запуск задачи через механизм возврата из прерывания
        : : : "r0", "memory"
    );
}

} // namespace embed