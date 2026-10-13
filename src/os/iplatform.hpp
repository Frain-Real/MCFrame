// iplatform.hpp
// ---------------------------------------------------------------------------
// ЕДИНСТВЕННЫЙ контракт между ядром ОС и платформой.
//
// Правила:
//   1. Этот заголовок НЕ включает ничего из ядра (кроме <cstdint>), чтобы
//      не возникало циклических зависимостей.
//   2. Платформа реализует РОВНО одну функцию embed::platform::ops().
//   3. Все указатели в Ops обязаны быть непустыми: незаполненное поле —
//      это ошибка конфигурации, которую ядро проверяет в os_boot().
//   4. noexcept является ЧАСТЬЮ ТИПА указателя (C++17), поэтому реализация
//      обязана объявлять функции ровно с этим квалификатором.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <cstddef>

namespace embed {

// Базовые типы объявлены здесь, а не в common.hpp, чтобы iplatform.hpp
// был самостоятельным и включался ПЕРВЫМ.
//
// Size — размер в байтах/словах. Это std::size_t, а НЕ unsigned int:
// на 64-битном хосте (mock-сборка) 32-битный размер не может адресовать
// память, и арифметика указателей в менеджере памяти становится неверной.
using Size      = std::size_t;
using Address   = uintptr_t;
using CoreId    = unsigned char;
using Priority  = unsigned char;
using Callback_h = void (*)(void* arg);

namespace platform {

/// Квалификатор для указателей на функции платформы.
#define OS_PLATFORM_FN noexcept

/// Набор операций платформы. Заполняется статически одной платформой.
struct Ops {
    // ---- Критическая секция -------------------------------------------------
    // enter() ОБЯЗАН вернуть предыдущее состояние маски прерываний
    // (PRIMASK / уровень вложенности), а exit() — восстановить его.
    // Возврат void недопустим: вложенные секции сломаются.
    uint32_t (*critical_enter)() OS_PLATFORM_FN;
    void     (*critical_exit)(uint32_t state) OS_PLATFORM_FN;

    // ---- Ядра ---------------------------------------------------------------
    uint32_t (*core_count)() OS_PLATFORM_FN;
    uint32_t (*current_core)() OS_PLATFORM_FN;

    // ---- Контекст задач -----------------------------------------------------
    void      (*start_first_task)(uint32_t* sp) OS_PLATFORM_FN;
    void      (*trigger_context_switch)() OS_PLATFORM_FN;
    uint32_t* (*save_context)() OS_PLATFORM_FN;
    void      (*load_context)(uint32_t* sp) OS_PLATFORM_FN;

    // ---- Время --------------------------------------------------------------
    void (*timer_init)() OS_PLATFORM_FN;
    bool (*timer_can_period_ms)(uint32_t period_ms) OS_PLATFORM_FN;
    void (*timer_set_period_ms)(uint32_t period_ms) OS_PLATFORM_FN;
    void (*timer_start)() OS_PLATFORM_FN;
    void (*timer_stop)() OS_PLATFORM_FN;

    // ---- Память: адреса приходят ОТ ПЛАТФОРМЫ (из линкера), не из ядра -----
    uint8_t*  (*heap_base)() OS_PLATFORM_FN;
    Size      (*heap_capacity)() OS_PLATFORM_FN;
    uint32_t* (*stack_pool_base)() OS_PLATFORM_FN;
    Size      (*stack_pool_capacity)() OS_PLATFORM_FN;   // в СЛОВАХ (uint32_t)

    // ---- Внешняя RAM --------------------------------------------------------
    bool      (*has_ext_ram)() OS_PLATFORM_FN;
    uint32_t* (*ext_ram_base)() OS_PLATFORM_FN;
    Size      (*ext_ram_capacity)() OS_PLATFORM_FN;      // в СЛОВАХ

    // ---- Внешняя ROM --------------------------------------------------------
    bool (*has_ext_rom)() OS_PLATFORM_FN;
    Size (*ext_rom_capacity)() OS_PLATFORM_FN;           // в БАЙТАХ
    bool (*ext_rom_read)(uint32_t offset, uint8_t* buf, uint32_t len) OS_PLATFORM_FN;
    bool (*ext_rom_write)(uint32_t offset, const uint8_t* buf, uint32_t len) OS_PLATFORM_FN;
    bool (*ext_rom_erase)(uint32_t offset, uint32_t len) OS_PLATFORM_FN;

    // ---- Стек задачи --------------------------------------------------------
    void      (*stack_paint)(uint32_t* stack, uint32_t words) OS_PLATFORM_FN;
    uint32_t* (*stack_init_frame)(uint32_t* sp, void (*fn)(void*), void* ctx) OS_PLATFORM_FN;

    // ---- Простой (энергосбережение) ----------------------------------------
    void (*idle)() OS_PLATFORM_FN;

    // ---- Привязка драйвера АЦП к обработчику прерывания --------------------
    // Ядро (драйвер ADC) регистрирует функцию-обработчик, платформа вызывает
    // её из своего IRQ. Так ADC1_2_IRQHandler не нужно «знать» про объект.
    void (*adc_bind)(void (*on_irq)() noexcept) OS_PLATFORM_FN;

    // ---- Лог ---------------------------------------------------------------
    // Платформенный вывод вместо stdio: printf на MCU тянет newlib-форматтер,
    // float-подсистему и заглушки _fstat/_sbrk/_isatty. Здесь строка уходит
    // туда, где ей место (SWO/ITM/UART), а на mock — в stdout.
    void (*log)(const char* str) OS_PLATFORM_FN;
};

/// Реализуется РОВНО одной платформой (mock/stm32/...).
const Ops& ops() OS_PLATFORM_FN;

/// Проверка полноты контракта. Возвращает false и печатает диагностику
/// (если доступна), если какое-то обязательное поле не заполнено.
bool validate() OS_PLATFORM_FN;

} // namespace platform
} // namespace embed

namespace embed {
    /// Хелпер: вывод строки через платформенный лог.
    /// Единственная точка логирования в ядре и примерах; stdio не тянется.
    inline void platform_log_line(const char* str) noexcept {
        if (str != nullptr && platform::ops().log != nullptr) {
            platform::ops().log(str);
        }
    }
} // namespace embed
