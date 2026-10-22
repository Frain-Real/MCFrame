// fault.cpp
// Обработчик аварии не гонится за скоростью: важнее уложиться в бюджет
// Flash (52 КБ слота). -Os даёт заметную экономию на форматировании отчёта.
#if defined(__GNUC__)
#pragma GCC optimize("Os")
#endif
#include "fault.hpp"

#include <cstddef>

/// Слабая реализация: в загрузчике (где нет планировщика) имени задачи нет.
/// Сильная реализация живёт в sheduler.cpp и перекрывает эту, если
/// планировщик слинкован. Определение обязано быть в глобальной области:
/// extern "C" внутри anonymous namespace получил бы внутреннюю линковку.
namespace embed {
extern "C" __attribute__((weak)) const char* mcframe_fault_task_name() noexcept {
    return nullptr;
}
} // namespace embed

namespace {

using embed::fault::Record;
using embed::fault::Info;
using embed::fault::Kind;

/// Запись в .noinit: startup зануляет только .bss, поэтому после reset
/// (но не после потери питания) здесь остаётся причина аварии.
#if defined(MCFRAME_TARGET_MCU)
__attribute__((section(".noinit"), used))
#endif
Record g_record{};

bool g_initialized = false;
bool g_had_fault   = false;

/// CRC-32 (IEEE 802.3, полином 0xEDB88320) по всей записи до поля crc.
uint32_t crc32(const uint8_t* data, embed::Size len) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    for (embed::Size i = 0u; i < len; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            crc = (crc & 1u) ? ((crc >> 1u) ^ 0xEDB88320u) : (crc >> 1u);
        }
    }
    return ~crc;
}

embed::Size record_crc_len() noexcept {
    return offsetof(Record, crc);
}

void update_crc(Record& r) noexcept {
    r.crc = crc32(reinterpret_cast<const uint8_t*>(&r), record_crc_len());
}

bool record_valid(const Record& r) noexcept {
    if (r.magic != embed::fault::kMagic) return false;
    if (r.version != embed::fault::kVersion) return false;
    const uint32_t want = crc32(reinterpret_cast<const uint8_t*>(&r),
                                record_crc_len());
    return want == r.crc;
}

/// Скопировать строку с усечением и гарантированным нулём.
void copy_text(char* dst, embed::Size cap, const char* src) noexcept {
    if (dst == nullptr || cap == 0u) return;
    embed::Size i = 0u;
    if (src != nullptr) {
        while (src[i] != '\0' && (i + 1u) < cap) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

const char* basename_of(const char* path) noexcept {
    if (path == nullptr) return nullptr;
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

void append_str(char*& p, char* end, const char* s) noexcept {
    if (s == nullptr) return;
    while (*s != '\0' && p < end) *p++ = *s++;
}

void append_hex(char*& p, char* end, uint32_t v) noexcept {
    static const char kHex[] = "0123456789ABCDEF";
    char tmp[8];
    for (uint32_t i = 0u; i < 8u; ++i) {
        tmp[7u - i] = kHex[v & 0xFu];
        v >>= 4u;
    }
    for (uint32_t i = 0u; i < 8u && p < end; ++i) *p++ = tmp[i];
}

void append_dec(char*& p, char* end, uint32_t v) noexcept {
    char tmp[10];
    uint32_t n = 0u;
    do {
        tmp[n++] = static_cast<char>('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && n < sizeof(tmp));
    while (n > 0u && p < end) *p++ = tmp[--n];
}

/// Сохранить снимок и завершить работу: без возврата в неопределённое
/// состояние ядра.
[[noreturn]] void die(Info& info) noexcept {
    embed::fault::capture(info);
    embed::fault::report();

    const embed::platform::Ops& ops = embed::platform::ops();
    if (ops.reset != nullptr) ops.reset();

    // Если платформа не умеет reset (или тестовая заглушка вернулась) —
    // остаёмся здесь: отладчик увидит точное место, а не «прыжок в мусор».
    for (;;) {
#if defined(__GNUC__)
        __asm volatile("" ::: "memory");
#endif
    }
}

} // namespace

namespace embed::fault {

bool init() noexcept {
    // Идемпотентность: если запись уже загружена и валидна — не перечитываем.
    // Если же записи нет (clear() или пустой .noinit), проверяем ещё раз:
    // BKP-метка могла появиться/обновиться.
    if (g_initialized && g_had_fault) return true;
    g_initialized = true;
    g_had_fault   = false;

    if (record_valid(g_record)) {
        g_had_fault = true;
        return true;
    }

    // Полной записи нет (потеря питания?), но BKP-метка переживает и её.
    uint16_t note = 0u;
    const platform::Ops& ops = platform::ops();
    if (ops.watchdog != nullptr && ops.watchdog->note_read != nullptr) {
        note = ops.watchdog->note_read(kNoteIndex);
    }
    if ((note & 0xFF00u) == kNoteMagic) {
        Record minimal{};
        minimal.magic   = kMagic;
        minimal.version = kVersion;
        minimal.kind    = static_cast<uint16_t>(note & 0x00FFu);
        copy_text(minimal.task, sizeof(minimal.task), "?");
        copy_text(minimal.where, sizeof(minimal.where), "bkp");
        update_crc(minimal);
        g_record    = minimal;
        g_had_fault = true;
        return true;
    }

    return false;
}

bool was_fault_reset() noexcept {
    if (!g_initialized) return init();
    return g_had_fault;
}

const Record* last() noexcept {
    if (!g_initialized) (void)init();
    return g_had_fault ? &g_record : nullptr;
}

void clear() noexcept {
    g_record      = Record{};
    g_initialized = true;
    g_had_fault   = false;

    const platform::Ops& ops = platform::ops();
    if (ops.watchdog != nullptr && ops.watchdog->note_write != nullptr) {
        (void)ops.watchdog->note_write(kNoteIndex, 0u);
    }
}

void capture(const Info& info) noexcept {
    Record r{};
    r.magic   = kMagic;
    r.version = kVersion;
    r.kind    = static_cast<uint16_t>(info.kind);
    r.tick    = info.tick;
    r.pc      = info.pc;
    r.lr      = info.lr;
    r.cfsr    = info.cfsr;

#if !defined(MCFRAME_FAULT_REPORT) || (MCFRAME_FAULT_REPORT)
    // Полный дамп нужен отчёту; в тихом режиме (slot) он только занимает
    // место в образе и в записи.
    r.psr   = info.psr;
    r.sp    = info.sp;
    r.r0    = info.r0;
    r.r1    = info.r1;
    r.r2    = info.r2;
    r.r3    = info.r3;
    r.r12   = info.r12;
    r.hfsr  = info.hfsr;
    r.mmfar = info.mmfar;
    r.bfar  = info.bfar;
    r.line  = info.line;
#endif

    const char* task = info.task;
    if (task == nullptr) task = mcframe_fault_task_name();
    copy_text(r.task, sizeof(r.task), task);
#if !defined(MCFRAME_FAULT_REPORT) || (MCFRAME_FAULT_REPORT)
    copy_text(r.where, sizeof(r.where), basename_of(info.file));
#endif

    update_crc(r);
    g_record      = r;
    g_initialized = true;
    g_had_fault   = true;

    // Короткая метка переживает потерю питания, когда .noinit уже потерян.
    // Полный PC туда не влезает, но «был Fault такого типа» — уже ответ.
    const platform::Ops& ops = platform::ops();
    if (ops.watchdog != nullptr && ops.watchdog->note_write != nullptr) {
        const uint16_t note = static_cast<uint16_t>(
            kNoteMagic | (static_cast<uint16_t>(info.kind) & 0x00FFu));
        (void)ops.watchdog->note_write(kNoteIndex, note);
    }
}

const char* kind_name(Kind k) noexcept {
    switch (k) {
        case Kind::HardFault:     return "HardFault";
        case Kind::MemManage:     return "MemManage";
        case Kind::BusFault:      return "BusFault";
        case Kind::UsageFault:    return "UsageFault";
        case Kind::Assert:        return "Assert";
        case Kind::Panic:         return "Panic";
        case Kind::StackOverflow: return "StackOverflow";
        case Kind::Watchdog:      return "Watchdog";
        case Kind::Unknown:
        default:                  return "Unknown";
    }
}

void report() noexcept {
#if !defined(MCFRAME_FAULT_REPORT) || (MCFRAME_FAULT_REPORT)
    if (!g_had_fault) {
        platform_log_line("MCFrame fault: no record");
        return;
    }

    const Record& r = g_record;
    char buf[112];

    char* p = buf;
    char* end = buf + sizeof(buf) - 1u;
    append_str(p, end, "MCFrame fault: ");
    append_str(p, end, kind_name(static_cast<Kind>(r.kind)));
    append_str(p, end, " task=");
    append_str(p, end, r.task[0] != '\0' ? r.task : "?");
    append_str(p, end, " at ");
    append_str(p, end, r.where[0] != '\0' ? r.where : "?");
    if (r.line != 0u) {
        append_str(p, end, ":");
        append_dec(p, end, r.line);
    }
    append_str(p, end, " pc=0x");
    append_hex(p, end, r.pc);
    append_str(p, end, " lr=0x");
    append_hex(p, end, r.lr);
    append_str(p, end, " cfsr=0x");
    append_hex(p, end, r.cfsr);
    *p = '\0';
    platform_log_line(buf);

#if defined(MCFRAME_FAULT_VERBOSE) && (MCFRAME_FAULT_VERBOSE)
    // Полный набор регистров нужен отладчику, но не бюджету слота в 52 КБ.
    // Включается -DMCFRAME_FAULT_VERBOSE=1, когда размер не критичен.
    p = buf;
    append_str(p, end, "  hfsr=0x");
    append_hex(p, end, r.hfsr);
    append_str(p, end, " mmfar=0x");
    append_hex(p, end, r.mmfar);
    append_str(p, end, " bfar=0x");
    append_hex(p, end, r.bfar);
    append_str(p, end, " sp=0x");
    append_hex(p, end, r.sp);
    *p = '\0';
    platform_log_line(buf);

    p = buf;
    append_str(p, end, "  r0=0x");
    append_hex(p, end, r.r0);
    append_str(p, end, " r1=0x");
    append_hex(p, end, r.r1);
    append_str(p, end, " r2=0x");
    append_hex(p, end, r.r2);
    append_str(p, end, " r3=0x");
    append_hex(p, end, r.r3);
    append_str(p, end, " r12=0x");
    append_hex(p, end, r.r12);
    *p = '\0';
    platform_log_line(buf);
#endif
#else
    // Тихий режим: запись сохранена, строка не печатается.
    return;
#endif
}

[[noreturn]] void panic(Kind kind, const char* file, uint32_t line) noexcept {
    Info info{};
    info.kind = kind;
    info.file = file;
    info.line = line;
    info.pc = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    info.sp = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&info));
    info.task = mcframe_fault_task_name();
    die(info);
}

[[noreturn]] void stack_overflow(const char* task) noexcept {
    Info info{};
    info.kind = Kind::StackOverflow;
    info.task = task;
    info.line = 0u;
    info.pc = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    info.sp = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&info));
    die(info);
}

} // namespace embed::fault
