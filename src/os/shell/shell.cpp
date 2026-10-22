// shell.cpp
#if defined(__GNUC__)
#pragma GCC optimize("Os")
#endif
#include "shell.hpp"

namespace {
bool starts_with(const char* s, const char* prefix, embed::Size n) noexcept {
    for (embed::Size i = 0u; i < n; ++i) {
        if (s[i] != prefix[i]) return false;
    }
    return true;
}
} // namespace

namespace embed::shell {

void Console::init(Writer writer, void* ctx) noexcept {
    write_ = writer;
    write_ctx_ = ctx;
    len_ = 0u;
    table_count_ = 0u;
    for (Size i = 0u; i < kMaxTables; ++i) {
        tables_[i] = nullptr;
        table_counts_[i] = 0u;
    }

    Size n = 0u;
    const Command* builtin = builtins(n);
    (void)add(builtin, n);
}

bool Console::add(const Command* commands, Size count) noexcept {
    if (commands == nullptr || count == 0u) return false;
    if (table_count_ >= kMaxTables) return false;
    tables_[table_count_] = commands;
    table_counts_[table_count_] = count;
    ++table_count_;
    return true;
}

const Command* Console::find(const char* name, Size name_len) const noexcept {
    for (Size t = 0u; t < table_count_; ++t) {
        const Command* table = tables_[t];
        for (Size i = 0u; i < table_counts_[t]; ++i) {
            const char* cmd = table[i].name;
            if (cmd == nullptr) continue;
            Size n = 0u;
            while (cmd[n] != '\0') ++n;
            if (n == name_len && starts_with(cmd, name, n)) return &table[i];
        }
    }
    return nullptr;
}

void Console::feed(char c) noexcept {
    if (c == '\r') return;

    if (c == '\n') {
        line_[len_] = '\0';
        execute(line_);
        len_ = 0u;
        prompt();
        return;
    }

    if (c == '\b' || c == 127) {
        if (len_ > 0u) --len_;
        return;
    }

    if (c >= 32 && c < 127 && (len_ + 1u) < kLineMax) {
        line_[len_++] = c;
    }
}

void Console::execute(const char* text) noexcept {
    if (text == nullptr) return;

    const char* p = text;
    while (*p == ' ') ++p;
    if (*p == '\0') return;

    const char* name = p;
    while (*p != '\0' && *p != ' ') ++p;
    const Size name_len = static_cast<Size>(p - name);

    while (*p == ' ') ++p;
    const char* args = p;

    const Command* cmd = find(name, name_len);
    if (cmd == nullptr) {
        write("unknown command: ");
        char tmp[Console::kLineMax];
        Size n = 0u;
        while (name[n] != '\0' && n + 1u < sizeof(tmp)) {
            tmp[n] = name[n];
            ++n;
        }
        tmp[n] = '\0';
        write(tmp);
        write("\n");
        return;
    }
    if (cmd->run != nullptr) cmd->run(*this, args);
}

void Console::write(const char* text) noexcept {
    if (write_ != nullptr && text != nullptr) write_(write_ctx_, text);
}

void Console::write_u32(uint32_t value) noexcept {
    char buf[11];
    uint32_t n = 0u;
    do {
        buf[n++] = static_cast<char>('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u && n < sizeof(buf));
    char out[11];
    for (uint32_t i = 0u; i < n; ++i) out[i] = buf[n - 1u - i];
    out[n] = '\0';
    write(out);
}

void Console::write_u64(uint64_t value) noexcept {
    if (value <= 0xFFFFFFFFu) {
        write_u32(static_cast<uint32_t>(value));
        return;
    }
    // Старшая часть отдельно: на F103 64-битное деление на 10 использует
    // библиотечную rutinu, но shell — не горячий путь.
    char buf[21];
    uint32_t n = 0u;
    do {
        buf[n++] = static_cast<char>('0' + static_cast<char>(value % 10u));
        value /= 10u;
    } while (value != 0u && n < sizeof(buf));
    char out[21];
    for (uint32_t i = 0u; i < n; ++i) out[i] = buf[n - 1u - i];
    out[n] = '\0';
    write(out);
}

void Console::write_hex(uint32_t value) noexcept {
    static const char kHex[] = "0123456789ABCDEF";
    char out[11];
    out[0] = '0';
    out[1] = 'x';
    for (uint32_t i = 0u; i < 8u; ++i) {
        out[2u + i] = kHex[(value >> (28u - 4u * i)) & 0xFu];
    }
    out[10] = '\0';
    write(out);
}

void Console::prompt() noexcept {
    write("> ");
}

void Console::help() noexcept {
    for (Size t = 0u; t < table_count_; ++t) {
        const Command* table = tables_[t];
        for (Size i = 0u; i < table_counts_[t]; ++i) {
            if (table[i].name == nullptr) continue;
            write("  ");
            write(table[i].name);
            if (table[i].help != nullptr) {
                write(" - ");
                write(table[i].help);
            }
            write("\n");
        }
    }
}

Size Console::command_count() const noexcept {
    Size n = 0u;
    for (Size t = 0u; t < table_count_; ++t) n += table_counts_[t];
    return n;
}

} // namespace embed::shell
