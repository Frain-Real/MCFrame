// shell.hpp
// ---------------------------------------------------------------------------
// Минималистичная командная консоль: строка → команда → вывод.
//
// НЕ ЗНАЕТ про UART. Приложение кормит её символами из своего цикла
// (uart.read(...)) и получает вывод через writer. Так shell не тянет
// драйверы в ядро и одинаково работает поверх UART, USB CDC или mock.
//
// Встроенные команды: help, ps, free, stats, log, reboot.
// `wd` и `ota` — отдельные опциональные заголовки, чтобы не тянуть
// watchdog/OTA в образ, которому они не нужны.
// ---------------------------------------------------------------------------
#pragma once

#include "../common.hpp"

namespace embed::shell {

using Writer = void (*)(void* ctx, const char* text) noexcept;

struct Command {
    const char* name;
    const char* help;
    void (*run)(class Console& console, const char* args);
};

class Console {
public:
    static constexpr Size kLineMax = 64;
    static constexpr Size kMaxTables = 4;

private:
    char         line_[kLineMax] = {};
    Size         len_ = 0u;
    Writer       write_ = nullptr;
    void*        write_ctx_ = nullptr;

    const Command* tables_[kMaxTables] = {};
    Size           table_counts_[kMaxTables] = {};
    Size           table_count_ = 0u;

    const Command* find(const char* name, Size name_len) const noexcept;

public:
    Console() noexcept = default;
    Console(const Console&)            = delete;
    Console& operator=(const Console&) = delete;

    /// Настроить вывод и добавить встроенные команды.
    void init(Writer writer, void* ctx) noexcept;

    /// Добавить таблицу команд. Указатель должен жить дольше консоли
    /// (обычно — статический массив).
    bool add(const Command* commands, Size count) noexcept;

    /// Подать один принятый символ. '\n' исполняет строку, '\b' стирает.
    void feed(char c) noexcept;

    /// Исполнить готовую строку (без завершающего перевода).
    void execute(const char* text) noexcept;

    void write(const char* text) noexcept;
    void write_u32(uint32_t value) noexcept;
    void write_u64(uint64_t value) noexcept;
    void write_hex(uint32_t value) noexcept;
    void prompt() noexcept;

    /// Напечатать список всех зарегистрированных команд.
    void help() noexcept;

    Size command_count() const noexcept;
};

/// Статическая таблица встроенных команд (определена в shell_builtins.cpp).
const Command* builtins(Size& count) noexcept;

} // namespace embed::shell
