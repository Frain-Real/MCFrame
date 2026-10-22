// log.cpp
#include "log.hpp"

#include "../core/task/sheduler.hpp"

namespace {

using embed::Size;
using embed::log::Entry;
using embed::log::Level;

Entry  g_ring[embed::log::kEntries]{};
Size   g_head = 0u;     // куда писать
Size   g_tail = 0u;     // что читать
Size   g_count = 0u;
uint8_t g_min_level = static_cast<uint8_t>(Level::Info);

void copy_text(char* dst, Size cap, const char* src) noexcept {
    if (dst == nullptr || cap == 0u) return;
    Size i = 0u;
    if (src != nullptr) {
        while (src[i] != '\0' && (i + 1u) < cap) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

/// Кольцо переполнено — вытесняем САМОЕ СТАРОЕ: после сбоя важнее последние
/// сообщения, а не первая строка загрузки.
void push_locked(const Entry& e) noexcept {
    if (g_count == embed::log::kEntries) {
        g_tail = (g_tail + 1u) % embed::log::kEntries;
        --g_count;
    }
    g_ring[g_head] = e;
    g_head = (g_head + 1u) % embed::log::kEntries;
    ++g_count;
}

bool level_allowed(Level level) noexcept {
    if (static_cast<uint8_t>(level) > static_cast<uint8_t>(g_min_level)) return false;
    if (static_cast<uint8_t>(level) > static_cast<uint8_t>(MCFRAME_LOG_LEVEL)) return false;
    return true;
}

Entry make_entry(Level level, const char* tag, const char* text) noexcept {
    Entry e{};
    e.tick  = static_cast<uint32_t>(embed::scheduler::instance().tick_count());
    e.level = level;
    copy_text(e.tag, sizeof(e.tag), tag);
    copy_text(e.text, sizeof(e.text), text);
    return e;
}

void append_str(char*& p, char* end, const char* s) noexcept {
    if (s == nullptr) return;
    while (*s != '\0' && p < end) *p++ = *s++;
}

void append_u32(char*& p, char* end, uint32_t v) noexcept {
    char tmp[10];
    uint32_t n = 0u;
    do {
        tmp[n++] = static_cast<char>('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && n < sizeof(tmp));
    while (n > 0u && p < end) *p++ = tmp[--n];
}

} // namespace

namespace embed::log {

void write(Level level, const char* tag, const char* text) noexcept {
    if (!level_allowed(level)) return;

    const Entry e = make_entry(level, tag, text);
    {
        Lock l;
        push_locked(e);
    }

    char buf[80];
    char* p = buf;
    char* end = buf + sizeof(buf) - 1u;
    append_str(p, end, "[");
    append_str(p, end, level_name(level));
    append_str(p, end, "] ");
    append_str(p, end, e.tag);
    append_str(p, end, ": ");
    append_str(p, end, e.text);
    *p = '\0';
    platform_log_line(buf);
}

void write_u32(Level level, const char* tag, const char* label,
               uint32_t value) noexcept {
    if (!level_allowed(level)) return;

    char buf[32];
    char* p = buf;
    char* end = buf + sizeof(buf) - 1u;
    if (label != nullptr && label[0] != '\0') {
        append_str(p, end, label);
        append_str(p, end, "=");
    }
    append_u32(p, end, value);
    *p = '\0';
    write(level, tag, buf);
}

void from_isr(Level level, const char* tag, const char* text) noexcept {
    if (!level_allowed(level)) return;
    const Entry e = make_entry(level, tag, text);
    Lock l;
    push_locked(e);
}

void set_level(Level level) noexcept {
    g_min_level = static_cast<uint8_t>(level);
}

Level level() noexcept {
    return static_cast<Level>(g_min_level);
}

bool pop(Entry& out) noexcept {
    Lock l;
    if (g_count == 0u) return false;
    out = g_ring[g_tail];
    g_tail = (g_tail + 1u) % kEntries;
    --g_count;
    return true;
}

Size pending() noexcept {
    Lock l;
    return g_count;
}

void clear() noexcept {
    Lock l;
    g_head = 0u;
    g_tail = 0u;
    g_count = 0u;
}

const char* level_name(Level level) noexcept {
    switch (level) {
        case Level::Error: return "ERR";
        case Level::Warn:  return "WRN";
        case Level::Info:  return "INF";
        case Level::Debug: return "DBG";
        default:           return "???";
    }
}

} // namespace embed::log
