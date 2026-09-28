#pragma once

#include "common.hpp"
#include <cstddef>
#include <type_traits>

namespace embed {
namespace utilities {
inline void* fast_memcpy(void* dst, const void* src, size_t len) {
    if (len == 0) return dst;
    
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);

    // Оптимизация: если оба указателя и длина выровнены по 4 байтам, 
    // копируем 32-битными словами (в 4 раза меньше циклов на MCU).
    if ((reinterpret_cast<uintptr_t>(d) % 4 == 0) &&
        (reinterpret_cast<uintptr_t>(s) % 4 == 0) &&
        (len % 4 == 0)) {
        
        uint32_t* d32 = reinterpret_cast<uint32_t*>(d);
        const uint32_t* s32 = reinterpret_cast<const uint32_t*>(s);
        size_t words = len >> 2; // деление на 4
        
        for (size_t i = 0; i < words; ++i) {
            d32[i] = s32[i];
        }
        return dst;
    }

    // Fallback: побайтовое копирование
    for (size_t i = 0; i < len; ++i) {
        d[i] = s[i];
    }
    return dst;
}

inline void* fast_memmove(void* dst, const void* src, size_t len) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);

    if (d == s || len == 0) return dst;

    if (d < s) {
        // Копируем вперед
        for (size_t i = 0; i < len; ++i) d[i] = s[i];
    } else {
        // Копируем назад (для перекрывающихся областей)
        for (size_t i = len; i > 0; --i) d[i - 1] = s[i - 1];
    }
    return dst;
}

inline void* fast_memset(void* dst, uint8_t value, size_t len) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < len; ++i) {
        d[i] = value;
    }
    return dst;
}

// Копирование одного объекта (структуры, union, POD)
template <typename T>
inline void copy(T& dst, const T& src) {
    static_assert(std::is_trivially_copyable_v<T>, 
                  "embed::utilities::copy: Type must be trivially copyable (no virtuals, no complex ctors)");
    fast_memcpy(&dst, &src, sizeof(T));
}

// Заполнение объекта значением (аналог memset для структур)
template <typename T>
inline void fill(T& obj, uint8_t value) {
    static_assert(std::is_trivially_copyable_v<T>, 
                  "embed::utilities::fill: Type must be trivially copyable");
    fast_memset(&obj, value, sizeof(T));
}

template <typename T>
inline void zero(T& obj) {
    fill(obj, 0);
}

// Копирование массива с проверкой границ на этапе компиляции и выполнения
template <typename T, size_t DstSize, size_t SrcSize>
inline Status copy_checked(T (&dst)[DstSize], const T (&src)[SrcSize]) {
    static_assert(std::is_trivially_copyable_v<T>, 
                  "embed::utilities::copy_checked: Type must be trivially copyable");
    if constexpr (SrcSize > DstSize) {
        return Status::NoMemory; 
    }

    // Если размеры динамические (но мы передаём их как ссылки на массивы, 
    // то они статические. Эта проверка для защиты от ручного приведения типов)
    if (SrcSize > DstSize) {
        return Status::NoMemory;
    }

    fast_memcpy(dst, src, SrcSize * sizeof(T));
    return Status::Ok;
}

// Копирование с явным указанием длины (для сырых буферов в драйверах)
template <typename T>
inline Status copy_buffer(T* dst, size_t dst_cap, const T* src, size_t src_len) {
    static_assert(std::is_trivially_copyable_v<T>, 
                  "embed::utilities::copy_buffer: Type must be trivially copyable");
                  
    if (dst == nullptr || src == nullptr) return Status::InvalidArg;
    if (src_len > dst_cap) return Status::NoMemory;
    if (src_len == 0) return Status::Ok;

    fast_memcpy(dst, src, src_len * sizeof(T));
    return Status::Ok;
}
}
}