// os/boot/boot_layout.hpp
#pragma once
// ---------------------------------------------------------------------------
// РАСКЛАДКА FLASH ДЛЯ СХЕМЫ «ЗАГРУЗЧИК + ДВА СЛОТА».
//
// ОДИН заголовок на два независимо собираемых образа — загрузчик и
// приложение. Так сделано намеренно: адреса слотов и области метаданных
// обязаны СОВПАДАТЬ у обоих, и «поправил в одном месте, забыл в другом» здесь
// означало бы, что загрузчик ищет образ не там, где его положили, или, что
// хуже, стирает область метаданных при записи слота.
//
// ───────────────────────────── СХЕМА STM32F103RB ────────────────────────────
//
//   0x08000000 ┌────────────────────────────┐
//              │ загрузчик (boot)           │  20 КБ: проверяет образ, считает
//              │                            │  попытки загрузки, передаёт
//   0x08005000 ├────────────────────────────┤  управление
//              │ слот A (app)               │  52 КБ: работающая прошивка
//              │                            │  (она же принимает обновление)
//   0x08012000 ├────────────────────────────┤
//              │ слот B (app)               │  52 КБ: сюда принимается новая
//              │                            │
//   0x0801F000 ├────────────────────────────┤
//              │ метаданные OTA             │  4 КБ: две копии записи на слот
//              │                            │
//   0x08020000 └────────────────────────────┘  128 КБ из 128 КБ
//
// Почему именно так:
//   * загрузчик занимает ПЕРВЫЕ килобайты: после сброса ядро всегда начинает
//     с 0x08000000, и «прыгать» на него не нужно ничем;
//   * слоты идут подряд и каждый начинается со страницы (1 КБ у STM32F1):
//     стирание идёт страницами, поэтому слот, начинающийся с середины
//     страницы, стереть нельзя;
//   * метаданные — в КОНЦЕ, чтобы стирание хвоста слота по ошибке не попало в
//     записи состояния (и наоборот: стирание записи не тронуло образ);
//   * оставшиеся 20 КБ (108…128) намеренно не заняты: там может лежать
//     «заводской» образ, настройки или просто запас, если слоты понадобится
//     увеличить.
//
// РАЗМЕРЫ ЗАВИСЯТ ОТ ПРИЛОЖЕНИЯ И ИЗМЕРЕНЫ, А НЕ ВЗЯТЫ «НА ГЛАЗ»
//   * загрузчик без приёма OTA — ≈19.1 КБ (отведено 20 КБ);
//   * загрузчик С приёмом OTA — ≈49 КБ (тогда 50 КБ на слот не остаётся:
//     на этой плате придётся выбирать между «залить прошивку в неработающее
//     устройство» и «двумя слотами с откатом»);
//   * «минимальное» приложение с полным приёмом OTA, планировщиком, сторожем и
//     задачами — ≈51 КБ (влезает в слот 52 КБ); полная «витрина» примера
//     занимает ≈61 КБ и в слот не влезает — поэтому демонстрации выключаются
//     (см. MCFRAME_APP_MINIMAL).
// Приложение, которому не хватает слота, надо уменьшать, а не двигать адреса
// наугад: раскладка обязана оставаться кратной странице, а слоты — не
// пересекаться.
// ---------------------------------------------------------------------------

#include "../common.hpp"

namespace embed::boot {

/// Пересекаются ли полуинтервалы [a0,a1) и [b0,b1).
constexpr bool overlaps(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) noexcept {
    return (a0 < b1) && (b0 < a1);
}

/// Раскладка образов в носителе.
struct Layout {
    /// Смещение и размер загрузчика (от начала носителя).
    uint32_t boot_offset = 0u;
    uint32_t boot_size   = 0u;

    /// Смещения и ёмкости слотов приложения.
    uint32_t slot_offset[2] = {0u, 0u};
    uint32_t slot_size[2]   = {0u, 0u};

    /// Область метаданных OTA (записи состояния слотов).
    uint32_t meta_offset = 0u;
    /// Сколько байт она занимает (4 страницы — по две копии на слот).
    uint32_t meta_size   = 0u;

    /// Пригодна ли раскладка для работы (проверяется загрузчиком при старте).
    ///
    /// Проверяются ровно те ошибки, которые иначе проявились бы посреди
    /// обновления: слот, налезающий на загрузчик (стирание себя), слоты друг
    /// на друге и метаданные внутри слота (стирание записи состояния вместе с
    /// образом).
    constexpr bool valid() const noexcept {
        if (boot_size == 0u || meta_size == 0u) return false;
        if (slot_size[0] == 0u || slot_size[1] == 0u) return false;
        if (slot_offset[0] < (boot_offset + boot_size)) return false;
        if (slot_offset[1] < (slot_offset[0] + slot_size[0])) return false;

        const uint32_t meta_end = meta_offset + meta_size;
        if (overlaps(meta_offset, meta_end, boot_offset, boot_offset + boot_size)) {
            return false;
        }
        for (uint8_t s = 0u; s < 2u; ++s) {
            if (overlaps(meta_offset, meta_end, slot_offset[s],
                         slot_offset[s] + slot_size[s])) {
                return false;
            }
        }
        return true;
    }

    /// Какой слот соответствует адресу (для приложения: по своему VTOR).
    constexpr uint8_t slot_of(uint32_t offset) const noexcept {
        for (uint8_t s = 0u; s < 2u; ++s) {
            if (slot_size[s] == 0u) continue;
            if (offset >= slot_offset[s] && offset < (slot_offset[s] + slot_size[s])) {
                return s;
            }
        }
        return kNoSlot;
    }

    /// Адрес слота в адресном пространстве носителя, если он есть.
    constexpr bool slot_address(uint8_t slot, uint32_t& out) const noexcept {
        if (slot > 1u || slot_size[slot] == 0u) return false;
        out = slot_offset[slot];
        return true;
    }

    /// Слот «нет» (ни один не подошёл).
    static constexpr uint8_t kNoSlot = 0xFFu;
};

/// Раскладка платы STM32F103RB (128 КБ Flash) из схемы выше.
inline constexpr Layout kStm32F103Rb = {
    /* boot_offset */ 0u,
    /* boot_size   */ 20u * 1024u,
    /* slot_offset */ {20u * 1024u, 72u * 1024u},
    /* slot_size   */ {52u * 1024u, 52u * 1024u},
    /* meta_offset */ 124u * 1024u,
    /* meta_size   */ 4u * 1024u,
};

/// Раскладка для стенда: та же арифметика, но «Flash» — модель носителя на
/// хосте. Отдельная константа, а не «та же самая»: на хосте размеры можно
/// уменьшить, чтобы тесты были быстрее, и важно, что это ЯВНОЕ решение.
inline constexpr Layout kMockStand = kStm32F103Rb;

// ─────────────────────────── Раскладка СБОРКИ ──────────────────────────────
//
// Числа приходят из CMake (cmake/firmware_layout.cmake) — из ОДНОГО места,
// откуда их же получает линкер. Загрузчик, приложение и линкер обязаны видеть
// одни и те же адреса: иначе загрузчик стирает слот, в котором лежит код
// приложения (или наоборот), и «странная ошибка» стоит прошивки.
//
// Без -D (например при чтении этого файла вне сборки) берётся схема для
// STM32F103RB из константы выше.

#ifndef MCFRAME_FLASH_BOOT_SIZE
#define MCFRAME_FLASH_BOOT_SIZE   (20u * 1024u)
#endif
#ifndef MCFRAME_FLASH_SLOT_SIZE
#define MCFRAME_FLASH_SLOT_SIZE   (52u * 1024u)
#endif
#ifndef MCFRAME_FLASH_SLOT0_OFFSET
#define MCFRAME_FLASH_SLOT0_OFFSET (20u * 1024u)
#endif
#ifndef MCFRAME_FLASH_SLOT1_OFFSET
#define MCFRAME_FLASH_SLOT1_OFFSET (72u * 1024u)
#endif
#ifndef MCFRAME_FLASH_META_OFFSET
#define MCFRAME_FLASH_META_OFFSET (124u * 1024u)
#endif
#ifndef MCFRAME_FLASH_META_SIZE
#define MCFRAME_FLASH_META_SIZE   (4u * 1024u)
#endif

/// Раскладка текущей сборки (совпадает с линкером и с загрузчиком).
inline constexpr Layout kBoard = {
    /* boot_offset */ 0u,
    /* boot_size   */ static_cast<uint32_t>(MCFRAME_FLASH_BOOT_SIZE),
    /* slot_offset */ {static_cast<uint32_t>(MCFRAME_FLASH_SLOT0_OFFSET),
                       static_cast<uint32_t>(MCFRAME_FLASH_SLOT1_OFFSET)},
    /* slot_size   */ {static_cast<uint32_t>(MCFRAME_FLASH_SLOT_SIZE),
                       static_cast<uint32_t>(MCFRAME_FLASH_SLOT_SIZE)},
    /* meta_offset */ static_cast<uint32_t>(MCFRAME_FLASH_META_OFFSET),
    /* meta_size   */ static_cast<uint32_t>(MCFRAME_FLASH_META_SIZE),
};

static_assert(kBoard.valid(), "раскладка сборки непригодна: слоты/метаданные пересекаются");

} // namespace embed::boot
