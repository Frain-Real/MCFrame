// utilities.hpp
#pragma once

#include "common.hpp"
#include <cstddef>
#include <type_traits>

namespace embed {
namespace utilities {

// ---------------------------------------------------------------------------
// Копирование / заполнение.
//
// Прежняя реализация содержала fast_memcpy/fast_memmove/fast_memset, которые
// «оптимизировали» копирование ручным разбором на слова и проверкой
// выравнивания через %. Это медленнее и объёмнее того, что генерирует GCC
// для memcpy/memset (LDM/STM на ARM), поэтому оставлены только обёртки над
// встроенными функциями.
// ---------------------------------------------------------------------------

inline void* memcpy_(void* dst, const void* src, std::size_t len) noexcept {
    return __builtin_memcpy(dst, src, len);
}

inline void* memmove_(void* dst, const void* src, std::size_t len) noexcept {
    return __builtin_memmove(dst, src, len);
}

inline void* memset_(void* dst, int value, std::size_t len) noexcept {
    return __builtin_memset(dst, value, len);
}

template <typename T>
inline int compare_(const T& a, const T& b) noexcept {
    return __builtin_memcmp(&a, &b, sizeof(T));
}

// --- Совместимость с прежними именами API ---
inline void* fast_memcpy(void* dst, const void* src, std::size_t len) noexcept {
    return memcpy_(dst, src, len);
}
inline void* fast_memmove(void* dst, const void* src, std::size_t len) noexcept {
    return memmove_(dst, src, len);
}
inline void* fast_memset(void* dst, uint8_t value, std::size_t len) noexcept {
    return memset_(dst, static_cast<int>(value), len);
}

/// Байтовое обнуление участка памяти, устойчивое к оптимизации.
/// Используется для затирания ключей: обычный memset оптимизатор вправе
/// выбросить, если память после этого не читается.
inline void secure_zero(void* dst, std::size_t len) noexcept {
    volatile uint8_t* p = static_cast<volatile uint8_t*>(dst);
    for (std::size_t i = 0; i < len; ++i) p[i] = 0u;
}

// ---------------------------------------------------------------------------
// Копирование объектов
// ---------------------------------------------------------------------------

/// Копирование одного тривиально копируемого объекта.
template <typename T>
inline void copy(T& dst, const T& src) noexcept {
    static_assert(std::is_trivially_copyable_v<T>,
                  "embed::utilities::copy: Type must be trivially copyable");
    memcpy_(&dst, &src, sizeof(T));
}

/// Заполнение объекта байтовым значением.
template <typename T>
inline void fill(T& obj, uint8_t value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>,
                  "embed::utilities::fill: Type must be trivially copyable");
    memset_(&obj, static_cast<int>(value), sizeof(T));
}

template <typename T>
inline void zero(T& obj) noexcept {
    fill(obj, 0u);
}

/// Копирование массива. Несоответствие размеров — ошибка КОМПИЛЯЦИИ,
/// а не рантайма: прежняя версия содержала недостижимую проверку
/// `if (SrcSize > DstSize)` сразу после такого же if constexpr.
template <typename T, std::size_t DstSize, std::size_t SrcSize>
inline Status copy_checked(T (&dst)[DstSize], const T (&src)[SrcSize]) noexcept {
    static_assert(std::is_trivially_copyable_v<T>,
                  "embed::utilities::copy_checked: Type must be trivially copyable");
    static_assert(SrcSize <= DstSize,
                  "embed::utilities::copy_checked: источник больше приёмника");
    memcpy_(dst, src, SrcSize * sizeof(T));
    return Status::Ok;
}

/// Копирование сырых буферов с проверкой ёмкости.
template <typename T>
inline Status copy_buffer(T* dst, std::size_t dst_cap,
                          const T* src, std::size_t src_len) noexcept {
    static_assert(std::is_trivially_copyable_v<T>,
                  "embed::utilities::copy_buffer: Type must be trivially copyable");
    if (dst == nullptr || src == nullptr) return Status::InvalidArg;
    if (src_len > dst_cap) return Status::NoMemory;
    if (src_len == 0u) return Status::Ok;
    memcpy_(dst, src, src_len * sizeof(T));
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// Псевдослучайные числа (xorshift32)
// ---------------------------------------------------------------------------

/**
 * @brief Контекст генератора. Хранится на стеке задачи или в статике модуля.
 */
struct os_rand_ctx_t {
    uint32_t state;
};

/**
 * @brief Инициализация ГПСЧ.
 * @param ctx   Указатель на контекст (nullptr игнорируется).
 * @param seed  Начальное значение (0 заменяется на 1: xorshift не терпит 0).
 * @return false, если ctx == nullptr.
 */
inline bool os_rand_init(os_rand_ctx_t* ctx, uint32_t seed) noexcept {
    if (ctx == nullptr) return false;
    ctx->state = (seed == 0u) ? 1u : seed;
    return true;
}

/**
 * @brief Следующее псевдослучайное число.
 */
inline uint32_t os_rand(os_rand_ctx_t* ctx) noexcept {
    if (ctx == nullptr) return 0u;
    uint32_t x = ctx->state;
    x ^= x << 13u;
    x ^= x >> 17u;
    x ^= x << 5u;
    ctx->state = x;
    return x;
}

/**
 * @brief Число в диапазоне [min, max] включительно.
 * @details Метод отклонения (rejection sampling) без modulo bias.
 */
inline uint32_t os_rand_range(os_rand_ctx_t* ctx, uint32_t min, uint32_t max) noexcept {
    if (ctx == nullptr) return min;
    if (min >= max) return min;

    // При min == 0 и max == UINT32_MAX диапазон переполняется: range == 0,
    // и вычисление UINT32_MAX % range привело бы к делению на ноль.
    if (min == 0u && max == 0xFFFFFFFFu) return os_rand(ctx);

    const uint32_t range = max - min + 1u;
    const uint32_t limit = 0xFFFFFFFFu - (0xFFFFFFFFu % range);

    uint32_t r;
    do {
        r = os_rand(ctx);
    } while (r >= limit);

    return min + (r % range);
}

} // namespace utilities
} // namespace embed
