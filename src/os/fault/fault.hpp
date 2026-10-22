// fault.hpp
// ---------------------------------------------------------------------------
// Посмертная диагностика отказов: Fault-обработчики, panic() и assert().
//
// ЗАЧЕМ ЭТОТ МОДУЛЬ
// На STM32F103 обработчики HardFault/MemManage/BusFault/UsageFault были
// слабыми алиасами на Default_Handler, то есть бесконечным циклом. На плате
// это выглядит как «зависло»: нет ни PC, ни задачи, ни причины. Для RTOS это
// самая дорогая из дешёвых ошибок — время на поиск уходит не на исправление,
// а на выяснение того, что вообще произошло.
//
// ЧТО ДЕЛАЕТ
//   * снимает кадр исключения (R0-R3, R12, LR, PC, xPSR) и регистры ядра
//     (CFSR/HFSR/MMFAR/BFAR);
//   * складывает компактную запись в .noinit (переживает reset, но не потерю
//     питания) и короткую метку в резервный регистр BKP (переживает и
//     потерю питания);
//   * печатает отчёт через платформенный лог;
//   * перезагружает MCU — внятно, а не в бесконечном цикле.
//
// ЧЕГО НЕ ДЕЛАЕТ
//   * не пытается «продолжить» исполнение после Fault: состояние ядра уже
//     не определено;
//   * не использует malloc/Lock/планировщик: обработчик обязан быть
//     безопасен в любой момент, в том числе до старта ОС.
// ---------------------------------------------------------------------------
#pragma once

#include "../common.hpp"
#include <cstdint>
#include <cstddef>

namespace embed {

/// Имя текущей задачи для отчёта. Реализуется планировщиком, если он
/// слинкован; в загрузчике остаётся слабая заглушка (nullptr), чтобы не
/// тянуть планировщик в 20-килобайтный образ.
extern "C" const char* mcframe_fault_task_name() noexcept;

namespace fault {

/// Причина аварии. Значения входят в BKP-метку, поэтому менять их
/// нумерацию нельзя: старая метка должна читаться новой прошивкой.
enum class Kind : uint32_t {
    Unknown       = 0u,
    HardFault     = 1u,
    MemManage     = 2u,
    BusFault      = 3u,
    UsageFault    = 4u,
    Assert        = 5u,
    Panic         = 6u,
    StackOverflow = 7u,
    Watchdog      = 8u
};

/// Магия записи: 'MCFA' (MCFrame Fault).
inline constexpr uint32_t kMagic   = 0x4D434641u;
inline constexpr uint16_t kVersion = 1u;

/// Индекс резервного регистра. 0..4 занимает watchdog (kNoteCount = 5),
/// 5..8 — OTA (note_base = 5). DR10 (индекс 9) оставлен под аварию.
/// Если приложение меняет note_base OTA, оно обязано не занять этот индекс.
inline constexpr uint32_t kNoteIndex = 9u;
inline constexpr uint16_t kNoteMagic = 0xFA00u;   // старший байт метки

/// Компактная запись. Лежит в .noinit: startup не зануляет её, поэтому после
/// reset (но не после потери питания) видно, что именно упало.
struct Record {
    uint32_t magic   = 0u;
    uint16_t version = 0u;
    uint16_t kind    = 0u;
    uint32_t tick    = 0u;     // тик ОС, если был известен
    uint32_t pc      = 0u;
    uint32_t lr      = 0u;
    uint32_t psr     = 0u;
    uint32_t sp      = 0u;
    uint32_t r0      = 0u;
    uint32_t r1      = 0u;
    uint32_t r2      = 0u;
    uint32_t r3      = 0u;
    uint32_t r12     = 0u;
    uint32_t cfsr    = 0u;
    uint32_t hfsr    = 0u;
    uint32_t mmfar   = 0u;
    uint32_t bfar    = 0u;
    uint32_t line    = 0u;
    char     task[8] = {};
    char     where[12] = {};
    uint32_t crc     = 0u;
};

/// Что снять с места аварии. Заполняет платформа (у неё есть SCB) или тест.
struct Info {
    Kind        kind  = Kind::Unknown;
    uint32_t    pc    = 0u;
    uint32_t    lr    = 0u;
    uint32_t    psr   = 0u;
    uint32_t    sp    = 0u;
    uint32_t    r0    = 0u;
    uint32_t    r1    = 0u;
    uint32_t    r2    = 0u;
    uint32_t    r3    = 0u;
    uint32_t    r12   = 0u;
    uint32_t    cfsr  = 0u;
    uint32_t    hfsr  = 0u;
    uint32_t    mmfar = 0u;
    uint32_t    bfar  = 0u;
    uint32_t    tick  = 0u;
    uint32_t    line  = 0u;
    const char* task  = nullptr;
    const char* file  = nullptr;
};

/// Загрузить запись после сброса. Возвращает true, если авария была.
/// Идемпотентна: повторный вызов не портит запись.
bool init() noexcept;

/// Была ли зафиксирована авария (в .noinit или в BKP-метке).
bool was_fault_reset() noexcept;

/// Последняя запись или nullptr, если аварии не было.
const Record* last() noexcept;

/// Стереть запись и BKP-метку (например, после успешного отчёта).
void clear() noexcept;

/// Сохранить снимок. Вызывается из обработчика или из panic().
/// Не сбрасывает MCU: это делает handle()/panic().
void capture(const Info& info) noexcept;

/// Напечатать отчёт через platform_log_line.
void report() noexcept;

/// Человекочитаемое имя причины.
const char* kind_name(Kind k) noexcept;

/// Аварийный сброс. НЕ возвращается.
[[noreturn]] void panic(Kind kind, const char* file, uint32_t line) noexcept;

/// Переполнение стека, обнаруженное в переключении контекста.
[[noreturn]] void stack_overflow(const char* task) noexcept;

} // namespace fault
} // namespace embed

/// Проверка инварианта. В отличие от стандартного assert не тянет stdio и
/// не «молчит»: пишет запись и перезагружает MCU.
#define MCFRAME_ASSERT(cond)                                                   \
    ((cond) ? static_cast<void>(0)                                             \
            : ::embed::fault::panic(::embed::fault::Kind::Assert,              \
                                    __FILE__, static_cast<uint32_t>(__LINE__)))

/// Безусловная авария с заданной причиной.
#define MCFRAME_PANIC(kind)                                                    \
    (::embed::fault::panic((kind), __FILE__, static_cast<uint32_t>(__LINE__)))
