#include "../../utilities.hpp"
#include "memory_manager.hpp"

extern "C"
void* malloc(std::size_t size) noexcept
{
    return embed::g_memory_manager.heap_alloc(static_cast<embed::Size>(size));
}
extern "C"
void free(void* ptr) noexcept
{
    embed::g_memory_manager.heap_free(ptr);
}
extern "C"
void* calloc(std::size_t count, std::size_t size) noexcept
{
    if (count != 0 &&
        size > static_cast<std::size_t>(-1) / count)
    {
        return nullptr;
    }

    const std::size_t total = count * size;

    void* ptr = embed::g_memory_manager.heap_alloc(static_cast<embed::Size>(total));

    if (ptr != nullptr)
    {
        embed::utilities::fill(ptr, 0);
    }

    return ptr;
}

void* operator new(std::size_t size)
{
    return  embed::g_memory_manager.heap_alloc(static_cast<embed::Size>(size));
}
void* operator new[](std::size_t size)
{
    return  embed::g_memory_manager.heap_alloc(static_cast<embed::Size>(size));
}

void operator delete(void* ptr) noexcept
{
    embed::g_memory_manager.heap_free(ptr);
}

void operator delete[](void* ptr) noexcept
{
    embed::g_memory_manager.heap_free(ptr);
}
void operator delete(void* ptr, std::size_t) noexcept
{
    embed::g_memory_manager.heap_free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
    embed::g_memory_manager.heap_free(ptr);
}