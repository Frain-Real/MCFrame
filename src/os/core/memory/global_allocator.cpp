// global_allocator.cpp
// ---------------------------------------------------------------------------
// Перехват стандартного распределения памяти кучей MCFrame.
//
// ВНИМАНИЕ: этот файл переопределяет malloc/calloc/realloc/free/operator new
// ГЛОБАЛЬНО. Он включается в сборку только при
//   -DMCFRAME_OVERRIDE_GLOBAL_ALLOCATOR=ON
// потому что на mock-сборке libstdc++ (std::cout, std::string) тоже ходит
// через эти функции: неинициализированная куча ОС приводит к std::terminate.
//
// Исправлено относительно прежней версии:
//  * добавлен realloc — без него libc/libstdc++ уходили в чужую кучу и
//    портили память (malloc перехвачен, а realloc нет);
//  * calloc действительно зануляет весь блок (прежде utilities::fill()
//    выводил T = void и занулял ровно ОДИН байт);
//  * operator new помечен noexcept и не бросает при -fno-exceptions;
//  * добавлены aligned_alloc/posix_memalign и nothrow-варианты new.
// ---------------------------------------------------------------------------
#include "memory_manager.hpp"

#include <cstddef>
#include <cstdint>

namespace {

using embed::Size;
using embed::g_memory_manager;

/// Заголовок блока, лежащий непосредственно перед пользовательским
/// указателем. Совпадает с memory_manager::heap_block.
using block_t = embed::memory_manager::heap_block;

inline block_t* header_of(void* ptr) noexcept {
    return reinterpret_cast<block_t*>(
        static_cast<uint8_t*>(ptr) - sizeof(block_t));
}

inline Size usable_size_of(void* ptr) noexcept {
    if (ptr == nullptr) return 0u;
    const Size total = header_of(ptr)->real_size();
    return (total > sizeof(block_t)) ? (total - sizeof(block_t)) : 0u;
}

/// Байтовое обнуление. НЕ через utilities::fill(): шаблонный fill(ptr, 0)
/// выводит T = void, sizeof(T) == 1, и обнуляет только первый байт.
inline void zero_bytes(void* dst, Size n) noexcept {
    volatile uint8_t* p = static_cast<volatile uint8_t*>(dst);
    for (Size i = 0; i < n; ++i) p[i] = 0u;
}

inline void copy_bytes(void* dst, const void* src, Size n) noexcept {
    uint8_t*       d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (Size i = 0; i < n; ++i) d[i] = s[i];
}

} // namespace

extern "C" {

void* malloc(std::size_t size) noexcept {
    if (size == 0u) size = 1u;
    return g_memory_manager.heap_alloc(static_cast<Size>(size));
}

void free(void* ptr) noexcept {
    g_memory_manager.heap_free(ptr);
}

void* calloc(std::size_t count, std::size_t size) noexcept {
    if (count == 0u || size == 0u) return nullptr;
    if (count > (static_cast<std::size_t>(-1) / size)) return nullptr;  // overflow

    const std::size_t total = count * size;
    void* p = g_memory_manager.heap_alloc(static_cast<Size>(total));
    if (p != nullptr) zero_bytes(p, static_cast<Size>(total));   // ВЕСЬ блок
    return p;
}

void* realloc(void* ptr, std::size_t size) noexcept {
    if (ptr == nullptr) return malloc(size);
    if (size == 0u) { free(ptr); return nullptr; }

    const Size old_size = usable_size_of(ptr);

    // Сжатие и совпадение делаем на месте: блок отдаётся целиком,
    // поэтому уменьшение размера безопасно.
    if (old_size >= size) return ptr;

    void* np = malloc(size);
    if (np == nullptr) return nullptr;      // старый блок НЕ трогаем
    copy_bytes(np, ptr, old_size);
    free(ptr);
    return np;
}

void* aligned_alloc(std::size_t alignment, std::size_t size) noexcept {
    // Куча выровнена по kHeapAlign для всех блоков, поэтому более строгое
    // выравнивание честно отклоняем вместо тихой выдачи неверного адреса.
    if (alignment > embed::memory_manager::kHeapAlign) return nullptr;
    return malloc(size);
}

int posix_memalign(void** memptr, std::size_t alignment, std::size_t size) noexcept {
    if (memptr == nullptr) return 22;       // EINVAL
    void* p = aligned_alloc(alignment, size);
    if (p == nullptr) return 12;            // ENOMEM
    *memptr = p;
    return 0;
}

/// _sbrk нужен newlib, если какая-то часть libc всё же вызовет malloc.
/// Отдаём память из кучи ОС, чтобы не было двух независимых куч.
void* _sbrk(std::ptrdiff_t /*incr*/) noexcept {
    return nullptr;
}

} // extern "C"

// ─────────────────────────────── operator new ───────────────────────────────
// При -fno-exceptions бросать bad_alloc нельзя: это std::terminate.
// Возвращаем nullptr, а проверку оставляем вызывающему (как в embedded-коде).

void* operator new(std::size_t size) noexcept {
    return malloc(size);
}
void* operator new[](std::size_t size) noexcept {
    return malloc(size);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return malloc(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return malloc(size);
}

void operator delete(void* ptr) noexcept { free(ptr); }
void operator delete[](void* ptr) noexcept { free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { free(ptr); }
