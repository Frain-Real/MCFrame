// log.hpp
// ---------------------------------------------------------------------------
// Логирование с уровнями и кольцевым буфером.
//
// ЗАЧЕМ: platform_log_line() умеет только строку без уровня, без метки и без
// истории. В поле нужно три вещи:
//   * отфильтровать Debug в релизе, не выкидывая Info;
//   * сохранить последние сообщения, чтобы shell показал их ПОСЛЕ события;
//   * писать из прерывания, не трогая медленный ITM/UART.
//
// Поэтому задача пишет в кольцо и сразу выводит строку, а ISR — только
// кладёт запись в кольцо (from_isr), не блокируясь на выводе.
// ---------------------------------------------------------------------------
#pragma once

#include "../common.hpp"
#include <cstdint>

#ifndef MCFRAME_LOG_ENTRIES
#define MCFRAME_LOG_ENTRIES 8u
#endif

/// Компиляторный порог: 0 — только Error, 3 — всё. Строки уровней выше
/// порога всё равно остаются в образе, но вызовы отбрасываются на входе.
#ifndef MCFRAME_LOG_LEVEL
#define MCFRAME_LOG_LEVEL 3
#endif

namespace embed::log {

enum class Level : uint8_t {
    Error = 0,
    Warn  = 1,
    Info  = 2,
    Debug = 3
};

inline constexpr Size kEntries = MCFRAME_LOG_ENTRIES;
static_assert(kEntries >= 1u, "MCFRAME_LOG_ENTRIES должен быть >= 1");

struct Entry {
    uint32_t tick = 0u;
    Level    level = Level::Info;
    char     tag[8] = {};
    char     text[48] = {};
};

/// Записать сообщение: в кольцо и в platform_log_line.
void write(Level level, const char* tag, const char* text) noexcept;

/// То же с числом: "label=123". label может быть nullptr.
void write_u32(Level level, const char* tag, const char* label,
               uint32_t value) noexcept;

/// Из прерывания: только в кольцо, без вывода (ITM/UART могут ждать).
void from_isr(Level level, const char* tag, const char* text) noexcept;

void  set_level(Level level) noexcept;
Level level() noexcept;

/// Достать самое старое сообщение. false — кольцо пусто.
bool pop(Entry& out) noexcept;
Size pending() noexcept;
void clear() noexcept;

const char* level_name(Level level) noexcept;

} // namespace embed::log

#define MCFRAME_LOG_ERROR(tag, msg)  ::embed::log::write(::embed::log::Level::Error, (tag), (msg))
#define MCFRAME_LOG_WARN(tag, msg)   ::embed::log::write(::embed::log::Level::Warn,  (tag), (msg))
#define MCFRAME_LOG_INFO(tag, msg)   ::embed::log::write(::embed::log::Level::Info,  (tag), (msg))
#define MCFRAME_LOG_DEBUG(tag, msg)  ::embed::log::write(::embed::log::Level::Debug, (tag), (msg))
