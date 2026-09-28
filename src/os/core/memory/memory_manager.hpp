#pragma once
#include "../../common.hpp"
#include "../task/task.hpp"
#include <cstdint>
#include <cstddef>

namespace embed {
    // Platform-dependent — реализация в platform.cpp
    uint32_t platform_max_heap_size() noexcept;
    uint32_t platform_max_stack_pool_size() noexcept;
    bool platform_has_ext_ram() noexcept;
    uint32_t* platform_ext_ram_base() noexcept;
    uint32_t platform_max_ext_ram_size() noexcept;
    bool platform_has_ext_rom() noexcept;
    uint32_t platform_max_ext_rom_size() noexcept;
    bool platform_ext_rom_read(uint32_t offset, uint8_t* buf, uint32_t len) noexcept;
    bool platform_ext_rom_write(uint32_t offset, const uint8_t* buf, uint32_t len) noexcept;
    bool platform_ext_rom_erase(uint32_t offset, uint32_t len) noexcept;
    void platform_stack_paint(uint32_t* stack, uint32_t words) noexcept;
    uint32_t* platform_stack_init_frame(uint32_t* sp, void (*fn)(void*), void* ctx) noexcept;

    /// Максимальное количество отслеживаемых задач.
    static constexpr uint32_t kMaxTrackedTasks = 32;
    class memory_manager {
    private:
        // ──── Heap ────
        struct heap_block {
            Size  size;   // размер блока (включая заголовок), 0 = свободен
            heap_block* next;  // следующий свободный блок (только если size == 0)
        };

        static constexpr Size kHeapAlign    = alignof(std::max_align_t);;
        static constexpr Size kHeapMinBlock = sizeof(heap_block);

        uint8_t*    heap_start_     = nullptr;
        Size   heap_size_      = 0;
        heap_block* heap_free_list_ = nullptr;

        // ──── Stack pool ────

        uint32_t*   stack_pool_base_ = nullptr;
        Size   stack_pool_size_ = 0;   // всего слов
        Size   stack_pool_used_ = 0;   // занято слов

        struct stack_slot {
            uint32_t*   base;    // начало стека (младший адрес)
            Size   words;   // размер в словах
            TaskHandler task;    // владелец (nullptr = свободен)
        };

        stack_slot  stack_slots_[kMaxTrackedTasks]{};
        Size   stack_slot_count_ = 0;

        // ──── Task tracking ────

        struct task_mem_info {
            TaskHandler task;
            uint32_t*   stack_base;
            Size   stack_words;
            Size   high_water;
        };

        task_mem_info tracked_tasks_[kMaxTrackedTasks]{};
        Size     tracked_count_ = 0;

        uint32_t*   ext_ram_base_     = nullptr;
        Size   ext_ram_size_     = 0;
        bool        ext_ram_active_   = false;

        Size   ext_rom_size_     = 0;
        bool        ext_rom_active_   = false;

        task_mem_info* find_tracked(TaskHandler t) noexcept;
        stack_slot*    find_stack_slot(uint32_t* base) noexcept;
        void           heap_merge_free() noexcept;

        Size align_up(Size n, Size align) const noexcept {
            return (n + align - 1) & ~(align - 1);
        }

    public:
        memory_manager() noexcept = default;
        memory_manager(const memory_manager&)            = delete;
        memory_manager& operator=(const memory_manager&) = delete;

        // ──────────── Init ────────────

        // Инициализировать менеджер памяти.
        // Платформа говорит максимумы (platform_max_*), пользователь —
        // сколько реально использовать. Значения обрезаются до максимумов.
        // heap_bytes        — байт под внутреннюю кучу (0 = не использовать).
        // stack_pool_words   — слов под пул стеков (0 = не использовать).
        // ext_ram_words      — слов внешней RAM (0 = не использовать).
        // ext_rom_bytes      — байт внешней ROM (0 = не использовать).
        void init(Size heap_bytes, Size stack_pool_words, Size ext_ram_words = 0, Size ext_rom_bytes = 0) noexcept;
        // ──────────── Stack pool ────────────
        uint32_t* stack_alloc(Size words) noexcept;

        // Вернуть стек в пул.
        void stack_free(uint32_t* stack) noexcept;

        // Зарегистрировать задачу и инициализировать стек.
        // Возвращает SP для первой загрузки.
        uint32_t* task_stack_init(TaskHandler t, uint32_t* stack, Size words) noexcept;

        /// Снять задачу с учёта.
        void task_unregister(TaskHandler t) noexcept;

        // ──────────── Heap ────────────
        /// Выделить из кучи (внутренняя + внешняя RAM). nullptr если не хватает.
        void* heap_alloc(Size bytes) noexcept;

        /// Вернуть в кучу.
        void heap_free(void* ptr) noexcept;

        // Monitoring
        Size task_stack_free(TaskHandler t) noexcept;
        Size task_stack_high_water(TaskHandler t) noexcept;
        bool task_stack_overflow(TaskHandler t) noexcept;
        void task_update_high_water(TaskHandler t) noexcept;
        void update_all_high_water() noexcept;

        // ──────────── Defragmentation ────────────
        void heap_defrag() noexcept;

        void stack_pool_defrag() noexcept;
        void defrag() noexcept;

        // ──────────── External ROM (файловое хранилище) ────────────
        bool ext_rom_active() const noexcept;
        // Прочитать данные из внешней ROM.
        bool ext_rom_read(uint32_t offset, uint8_t* buf, uint32_t len) noexcept;
        // Записать данные во внешнюю ROM.
        bool ext_rom_write(uint32_t offset, const uint8_t* buf, uint32_t len) noexcept;
        // Стереть сектор внешней ROM.
        bool ext_rom_erase(uint32_t offset, uint32_t len) noexcept;

        // ──────────── Stats ────────────
        Size heap_total()      const noexcept { return heap_size_; }
        Size heap_free_bytes() const noexcept {
            Size free_mem = 0;
            heap_block* cur = heap_free_list_;
            while (cur) {
                if (cur->size == 0) {
                    uint8_t* block_end = heap_start_ + heap_size_;
                    uint8_t* cur_ptr   = reinterpret_cast<uint8_t*>(cur);
                    if (ext_ram_active_ && cur_ptr >= reinterpret_cast<uint8_t*>(ext_ram_base_)) {
                        block_end = reinterpret_cast<uint8_t*>(ext_ram_base_ + ext_ram_size_);
                    }
                    free_mem += static_cast<Size>(block_end - cur_ptr);
                }
                cur = cur->next;
            }
            return free_mem;
        }

        Size heap_used() const noexcept { return heap_size_ - heap_free_bytes(); }

        Size stack_pool_total() const noexcept { return stack_pool_size_; }
        Size stack_pool_used()  const noexcept { return stack_pool_used_; }
        Size stack_pool_free()  const noexcept { return stack_pool_size_ - stack_pool_used_; }

        Size ext_ram_total()    const noexcept { return ext_ram_size_; }
        bool ext_ram_active()   const noexcept { return ext_ram_active_; }

        Size ext_rom_total()    const noexcept { return ext_rom_size_; }

        Size tracked_count()    const noexcept { return tracked_count_; }

    private:
        static constexpr uint32_t kStackMagic = 0xDEADBEEF;
        static constexpr uint32_t kStackFill  = 0xA5A5A5A5;
    };

    inline memory_manager g_memory_manager{};
}
