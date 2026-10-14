// crypto_utils.hpp
// ---------------------------------------------------------------------------
// Общие примитивы модуля криптографии: доступ к байтам в фиксированном
// порядке (big/little endian), сравнение за постоянное время, PKCS#7 и
// затирание секретов.
//
// ПОЧЕМУ ОТДЕЛЬНЫЙ ФАЙЛ: прежде каждый модуль (sha2/sha3/aes/tdes/ecc)
// собирал байты из слов вручную, и это дублирование уже дало расхождения
// (перепутанные порядки байт, ручные циклы с неверными индексами).
// ---------------------------------------------------------------------------
#pragma once

#include "../common.hpp"
#include "../utilities.hpp"

#include <cstddef>
#include <cstdint>

namespace embed {
namespace crypto {

// ---------------------------------------------------------------------------
// Загрузка/сохранение слов в фиксированном порядке байт.
// Все стандартные хэши и шифры задают порядок байт явно, поэтому
// memcpy структуры на слово здесь неприменим.
// ---------------------------------------------------------------------------

inline uint32_t load_be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
            static_cast<uint32_t>(p[3]);
}

inline void store_be32(uint32_t v, uint8_t* p) noexcept {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

inline uint64_t load_be64(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(load_be32(p)) << 32) |
            static_cast<uint64_t>(load_be32(p + 4));
}

inline void store_be64(uint64_t v, uint8_t* p) noexcept {
    store_be32(static_cast<uint32_t>(v >> 32), p);
    store_be32(static_cast<uint32_t>(v), p + 4);
}

inline uint32_t load_le32(const uint8_t* p) noexcept {
    return  static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline void store_le32(uint32_t v, uint8_t* p) noexcept {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

inline uint64_t load_le64(const uint8_t* p) noexcept {
    return  static_cast<uint64_t>(load_le32(p)) |
           (static_cast<uint64_t>(load_le32(p + 4)) << 32);
}

inline void store_le64(uint64_t v, uint8_t* p) noexcept {
    store_le32(static_cast<uint32_t>(v), p);
    store_le32(static_cast<uint32_t>(v >> 32), p + 4);
}

// ---------------------------------------------------------------------------
// Сравнение за постоянное время.
//
// Обычный memcmp выходит из цикла на первом различии: по времени работы
// восстанавливается позиция первого несовпавшего байта (MAC/тег). Здесь
// накапливается XOR-разница и время не зависит от данных.
// ---------------------------------------------------------------------------
inline bool ct_equal(const uint8_t* a, const uint8_t* b, Size len) noexcept {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    uint8_t diff = 0u;
    for (Size i = 0; i < len; ++i) {
        diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    }
    return diff == 0u;
}

/// Затирание секретов: memset оптимизатор вправе выбросить.
inline void wipe(void* p, Size len) noexcept {
    utilities::secure_zero(p, len);
}

template <typename T>
inline void wipe_object(T& obj) noexcept {
    wipe(&obj, sizeof(T));
}

// ---------------------------------------------------------------------------
// PKCS#7 (RFC 5652, §6.3) — общий для блочных шифров модуля.
//
// Прежний код имел ДВЕ независимые реализации (AES и TDES) с разными
// правилами проверки, из-за чего одна и та же операция вела себя по-разному.
// ---------------------------------------------------------------------------

/// Размер после добавления padding'а (всегда кратен block_size, минимум +1).
constexpr Size pkcs7_padded_size(Size len, Size block_size) noexcept {
    return len + (block_size - (len % block_size));
}

/// Число байтов padding'а для длины len (1..block_size).
constexpr Size pkcs7_padding_len(Size len, Size block_size) noexcept {
    return block_size - (len % block_size);
}

/// Добавить padding в буфер data (ёмкость cap байт).
/// @return длина с padding'ом или 0, если буфер мал/аргументы неверны.
inline Size pkcs7_pad(uint8_t* data, Size len, Size cap, Size block_size) noexcept {
    if (data == nullptr || block_size == 0u || block_size > 255u) return 0u;
    const Size pad = pkcs7_padding_len(len, block_size);
    if (len + pad > cap) return 0u;
    for (Size i = 0; i < pad; ++i) {
        data[len + i] = static_cast<uint8_t>(pad);
    }
    return len + pad;
}

/// Убрать padding. Длина без padding'а возвращается через out_len.
/// @return false, если длина не кратна блоку или padding некорректен.
///         Нулевая длина сообщения — это успех (out_len = 0), а не ошибка.
inline bool pkcs7_unpad(const uint8_t* data, Size len, Size block_size,
                        Size* out_len) noexcept {
    if (data == nullptr || out_len == nullptr) return false;
    if (block_size == 0u || block_size > 255u) return false;
    if (len == 0u || len % block_size != 0u) return false;

    const uint8_t pad = data[len - 1u];
    if (pad == 0u || pad > block_size || static_cast<Size>(pad) > len) return false;

    // Проверяем все байты padding'а, не выходя за границы и не «укорачивая»
    // цикл на первом несовпадении (иначе по времени видно корректность).
    uint8_t diff = 0u;
    for (Size i = 0; i < pad; ++i) {
        diff = static_cast<uint8_t>(diff | (data[len - 1u - i] ^ pad));
    }
    if (diff != 0u) return false;

    *out_len = len - pad;
    return true;
}

} // namespace crypto
} // namespace embed
