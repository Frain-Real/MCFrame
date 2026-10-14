// platforms/stm32/flash_stm32.cpp
// ---------------------------------------------------------------------------
// Встроенная Flash STM32F1 (RM0008, раздел 3.3): unlock, стирание страницы,
// программирование half-word. См. flash_stm32.hpp — там о порядке шагов и о
// том, почему mass erase и option bytes сюда не входят.
//
// ВНИМАНИЕ: этот файл исполняется ИЗ ТОЙ ЖЕ Flash, которую стирает. Стирание
// страницы, содержащей код, — это мгновенный HardFault (или «тихий» уход в
// мусор при выборке следующей инструкции). Поэтому здесь нет и не должно быть
// ничего, что стирает по адресу, который не проверил вызывающий: разделение
// «политика в os/ota» — «регистры здесь» существует ровно ради этого.
// ---------------------------------------------------------------------------
#include "flash_stm32.hpp"
#include "stm32_regs.hpp"

#include <cstring>

// Размер Flash, если регистр размера не читается. Значение по умолчанию —
// МИНИМАЛЬНОЕ из ходовых (64 КБ): при нечитаемом регистре лучше отказать в
// записи ближе к концу Flash, чем разрешить её за пределами кристалла.
// Для 128 КБ: -DMCFRAME_STM32_FLASH_KB=128.
#ifndef MCFRAME_STM32_FLASH_KB
#define MCFRAME_STM32_FLASH_KB 64u
#endif

namespace embed::stm32::flash {

namespace {

constexpr uint32_t kKey1 = 0x45670123u;
constexpr uint32_t kKey2 = 0xCDEF89ABu;

constexpr uint32_t kPageSize = 1024u;

/// Кэш размера Flash: читается из регистра размера один раз.
uint32_t g_capacity_bytes = 0u;

/// Адрес регистра размера Flash у STM32F1 (F_SIZE).
constexpr uint32_t kFlashSizeReg = 0x1FFFF7E0u;

bool wait_not_busy() noexcept {
    for (uint32_t i = 0u; i < kBusyTimeoutLoops; ++i) {
        if ((FLASH->SR & FLASH_SR_BSY) == 0u) return true;
    }
    return false;
}

/// Снять флаги завершения и ошибок. У FLASH_SR они снимаются ЗАПИСЬЮ ЕДИНИЦЫ
/// (rc_w1), поэтому «просто прочитать» их недостаточно: флаг PGERR остался бы
/// выставленным и следующий вызов принял бы его за свежую ошибку.
void clear_flags() noexcept {
    FLASH->SR = static_cast<uint32_t>(FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR);
}

bool has_error() noexcept {
    return (FLASH->SR & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) != 0u;
}

/// Записать half-word по абсолютному адресу. Возврат — успех операции.
bool program_halfword(uint32_t addr, uint16_t value) noexcept {
    if (!wait_not_busy()) return false;

    FLASH->CR |= FLASH_CR_PG;

    // Именно 16-битная запись: на STM32F1 попытка записать 32 бита даёт PGERR
    // (в medium density программирование half-word).
    *reinterpret_cast<volatile uint16_t*>(addr) = value;

    if (!wait_not_busy()) {
        FLASH->CR &= static_cast<uint16_t>(~FLASH_CR_PG);
        return false;
    }
    FLASH->CR &= static_cast<uint16_t>(~FLASH_CR_PG);

    const bool ok = !has_error();
    clear_flags();
    return ok;
}

uint16_t read_halfword(uint32_t addr) noexcept {
    return *reinterpret_cast<volatile const uint16_t*>(addr);
}

} // namespace

// ───────────────────────────────── Геометрия ─────────────────────────────────

uint32_t page_size() noexcept { return kPageSize; }
uint32_t write_granularity() noexcept { return 2u; }
uint32_t base() noexcept { return FLASH_BASE; }

uint32_t capacity_bytes() noexcept {
    if (g_capacity_bytes != 0u) return g_capacity_bytes;

    const uint16_t kb = *reinterpret_cast<volatile const uint16_t*>(kFlashSizeReg);
    // 0xFFFF на этом регистре означает «не прочитался» (клоны/защита):
    // доверять такому значению нельзя — это была бы Flash на 64 МБ.
    // Регистр не прочитался (клоны, «залипшее» значение) — берём МИНИМАЛЬНЫЙ
    // вариант, а не максимальный: разрешить запись за пределами реальной Flash
    // хуже, чем отказать в законной операции. Точное значение задаётся сборкой.
    g_capacity_bytes = (kb == 0xFFFFu || kb == 0u)
                     ? static_cast<uint32_t>(MCFRAME_STM32_FLASH_KB) * 1024u
                     : static_cast<uint32_t>(kb) * 1024u;
    return g_capacity_bytes;
}

// ──────────────────────────────── Защита ────────────────────────────────────

bool locked() noexcept { return (FLASH->CR & FLASH_CR_LOCK) != 0u; }

bool unlock() noexcept {
    if (!locked()) return true;

    FLASH->KEYR = kKey1;
    FLASH->KEYR = kKey2;

    // Оба ключа подряд и ровно один раз: если после них LOCK всё ещё взведён,
    // значит последовательность была нарушена (или защита включена опциями) —
    // второй попытки не делаем, её пришлось бы начинать с KEY1.
    return !locked();
}

void lock() noexcept {
    if (!wait_not_busy()) return;   // операция идёт: взводить LOCK сейчас нельзя
    FLASH->CR |= FLASH_CR_LOCK;
}

// ──────────────────────────────── Стирание ──────────────────────────────────

bool erase(uint32_t addr, uint32_t len) noexcept {
    const uint32_t cap = capacity_bytes();
    if (len == 0u || (len % kPageSize) != 0u) return false;
    if ((addr % kPageSize) != 0u) return false;
    if (addr < base()) return false;
    if (len > (base() + cap - addr)) return false;      // без переполнения суммы
    // При взведённом LOCK запись в CR.PER/CR.STRT игнорируется, флаги ошибок не
    // выставляются, и стирание «проходит успешно», ничего не стерев. Молчаливый
    // успех здесь — худший из возможных ответов.
    if (locked()) return false;

    for (uint32_t off = 0u; off < len; off += kPageSize) {
        if (!wait_not_busy()) return false;

        clear_flags();
        FLASH->CR |= FLASH_CR_PER;
        FLASH->AR  = addr + off;
        FLASH->CR |= FLASH_CR_STRT;

        const bool done = wait_not_busy();
        FLASH->CR &= static_cast<uint16_t>(~FLASH_CR_PER);

        if (!done) return false;
        if (has_error()) {
            clear_flags();
            return false;
        }
        clear_flags();
    }
    return true;
}

// ──────────────────────────── Программирование ──────────────────────────────

bool program(uint32_t addr, const void* data, uint32_t len) noexcept {
    if (data == nullptr && len != 0u) return false;
    if (len == 0u) return true;
    if (locked()) return false;

    const uint32_t cap = capacity_bytes();
    if (addr < base()) return false;
    if (len > (base() + cap - addr)) return false;      // без переполнения суммы

    const auto* src = static_cast<const uint8_t*>(data);
    uint32_t pos = 0u;

    // ── Голова: адрес не выровнен по half-word ──
    //
    // Дописать половину пары можно ТОЛЬКО если второй её байт ещё стёрт
    // (0xFF), то есть ячейка не программировалась. Это не перестраховка:
    // повторное программирование уже записанной ячейки спецификацией F1 не
    // разрешено (для изменения — стереть страницу), и «получилось или нет»
    // зависит от кристалла. Поэтому в остальных случаях — честный отказ:
    // вызывающий обязан писать кратными порциями (сервис OTA так и делает,
    // см. os/ota, stage_bytes/flush_pending).
    if ((addr & 1u) != 0u) {
        const uint16_t old_pair = read_halfword(addr & ~1u);
        if ((old_pair & 0x00FFu) != 0x00FFu) return false;
        const uint16_t merged = static_cast<uint16_t>((old_pair & 0x00FFu) |
                                 (static_cast<uint16_t>(src[0]) << 8));
        if (!program_halfword(addr & ~1u, merged)) return false;
        pos = 1u;
    }

    // ── Середина: пары байт ──
    while ((len - pos) >= 2u) {
        const uint16_t value = static_cast<uint16_t>(
            static_cast<uint16_t>(src[pos]) |
            (static_cast<uint16_t>(src[pos + 1u]) << 8));
        if (!program_halfword(addr + pos, value)) return false;
        pos += 2u;
    }

    // ── Хвост: остался один байт, его пара выходит за записываемый участок ──
    if (pos < len) {
        const uint32_t tail_addr = addr + pos;
        const uint16_t old_pair = read_halfword(tail_addr & ~1u);
        if ((old_pair & 0xFF00u) != 0xFF00u) return false;   // байт уже записан
        const uint16_t merged = static_cast<uint16_t>((old_pair & 0xFF00u) | src[pos]);
        if (!program_halfword(tail_addr & ~1u, merged)) return false;
    }
    return true;
}

// ──────────────────────────────── Чтение ────────────────────────────────────

bool read(uint32_t addr, void* data, uint32_t len) noexcept {
    if (data == nullptr && len != 0u) return false;
    const uint32_t cap = capacity_bytes();
    if (addr < base()) return false;
    if (len > (base() + cap - addr)) return false;      // без переполнения суммы

    // Flash отображена в адресное пространство: чтение — обычная память.
    std::memcpy(data, reinterpret_cast<const void*>(addr), len);
    return true;
}

// ──────────────────────────── Таблица для ядра ──────────────────────────────

namespace {

uint32_t ops_caps() OS_PLATFORM_FN {
    return static_cast<uint32_t>(platform::FlashCaps::Internal) |
           static_cast<uint32_t>(platform::FlashCaps::Lockable) |
           static_cast<uint32_t>(platform::FlashCaps::ErasePage);
}
uint32_t ops_base() OS_PLATFORM_FN { return base(); }
uint32_t ops_capacity() OS_PLATFORM_FN { return capacity_bytes(); }
uint32_t ops_page_size() OS_PLATFORM_FN { return page_size(); }
uint32_t ops_granularity() OS_PLATFORM_FN { return write_granularity(); }
bool     ops_unlock() OS_PLATFORM_FN { return unlock(); }
void     ops_lock() OS_PLATFORM_FN { lock(); }
bool     ops_erase(uint32_t addr, uint32_t len) OS_PLATFORM_FN { return erase(addr, len); }
bool     ops_program(uint32_t addr, const void* data, uint32_t len) OS_PLATFORM_FN {
    return program(addr, data, len);
}
bool     ops_read(uint32_t addr, void* data, uint32_t len) OS_PLATFORM_FN {
    return read(addr, data, len);
}

const platform::FlashOps kOps{
    /* caps              */ &ops_caps,
    /* base              */ &ops_base,
    /* capacity          */ &ops_capacity,
    /* page_size         */ &ops_page_size,
    /* write_granularity */ &ops_granularity,
    /* unlock            */ &ops_unlock,
    /* lock              */ &ops_lock,
    /* erase             */ &ops_erase,
    /* program           */ &ops_program,
    /* read              */ &ops_read,
};

} // namespace

const platform::FlashOps& ops() noexcept { return kOps; }

} // namespace embed::stm32::flash
