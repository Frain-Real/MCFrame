// memory_manager.hpp
#pragma once

#include "../task.hpp"
#include <cstdint>

namespace embed {

    // ════════════════════════════════════════════════════════════
    // Platform-dependent — реализация в platform.cpp
    // ════════════════════════════════════════════════════════════

    /// Максимальный размер внутренней кучи (байт). 0 = нет.
    uint32_t platform_max_heap_size() noexcept;

    /// Максимальный размер пула стеков (слов uint32_t). 0 = нет.
    uint32_t platform_max_stack_pool_size() noexcept;

    /// Есть ли возможность подключить внешнюю RAM.
    bool platform_has_ext_ram() noexcept;

    /// Базовый адрес внешней RAM (физический). nullptr = нет.
    uint32_t* platform_ext_ram_base() noexcept;

    /// Максимальный размер внешней RAM (слов uint32_t).
    uint32_t platform_max_ext_ram_size() noexcept;

    /// Есть ли возможность подключить внешнюю ROM.
    bool platform_has_ext_rom() noexcept;

    /// Максимальный размер внешней ROM (байт).
    uint32_t platform_max_ext_rom_size() noexcept;

    /// Прочитать из внешней ROM.
    /// ⚠️ Реализация — в platform.cpp.
    bool platform_ext_rom_read(uint32_t offset, uint8_t* buf, uint32_t len) noexcept;

    /// Записать во внешнюю ROM.
    /// ⚠️ Реализация — в platform.cpp.
    bool platform_ext_rom_write(uint32_t offset, const uint8_t* buf, uint32_t len) noexcept;

    /// Стереть сектор внешней ROM.
    /// ⚠️ Реализация — в platform.cpp.
    bool platform_ext_rom_erase(uint32_t offset, uint32_t len) noexcept;

    /// Залить стек известным паттерном (для отслеживания использования).
    /// ⚠️ Реализация — в platform.cpp.
    void platform_stack_paint(uint32_t* stack, uint32_t words) noexcept;

    /// Инициализировать exception frame на вершине стека.
    /// ⚠️ Реализация — в platform.cpp (зависит от архитектуры).
    uint32_t* platform_stack_init_frame(uint32_t* sp, void (*fn)(void*), void* ctx) noexcept;

    // ════════════════════════════════════════════════════════════
    // Memory Manager
    // ════════════════════════════════════════════════════════════

    /// Максимальное количество отслеживаемых задач.
    static constexpr uint32_t kMaxTrackedTasks = 16;

    /// Менеджер памяти: куча, пул стеков, внешняя RAM/ROM, трекинг задач.
    class memory_manager {
    public:
        using size_type = uint32_t;

    private:
        // ──── Heap ────

        struct heap_block {
            size_type  size;   // размер блока (включая заголовок), 0 = свободен
            heap_block* next;  // следующий свободный блок (только если size == 0)
        };

        static constexpr size_type kHeapAlign    = 4;
        static constexpr size_type kHeapMinBlock = sizeof(heap_block);

        uint8_t*    heap_start_     = nullptr;
        size_type   heap_size_      = 0;
        heap_block* heap_free_list_ = nullptr;

        // ──── Stack pool ────

        uint32_t*   stack_pool_base_ = nullptr;
        size_type   stack_pool_size_ = 0;   // всего слов
        size_type   stack_pool_used_ = 0;   // занято слов

        struct stack_slot {
            uint32_t*   base;    // начало стека (младший адрес)
            size_type   words;   // размер в словах
            TaskHandler task;    // владелец (nullptr = свободен)
        };

        stack_slot  stack_slots_[kMaxTrackedTasks]{};
        size_type   stack_slot_count_ = 0;

        // ──── Task tracking ────

        struct task_mem_info {
            TaskHandler task;
            uint32_t*   stack_base;
            size_type   stack_words;
            size_type   high_water;  // минимум свободных слов
        };

        task_mem_info tracked_tasks_[kMaxTrackedTasks]{};
        size_type     tracked_count_ = 0;

        // ──── External RAM ────

        uint32_t*   ext_ram_base_     = nullptr;
        size_type   ext_ram_size_     = 0;   // слов (сколько реально используется)
        bool        ext_ram_active_   = false;

        // ──── External ROM ────

        size_type   ext_rom_size_     = 0;   // байт (сколько реально используется)
        bool        ext_rom_active_   = false;

        // ──── Internal helpers ────

        task_mem_info* find_tracked(TaskHandler t) noexcept;
        stack_slot*    find_stack_slot(uint32_t* base) noexcept;
        void           heap_merge_free() noexcept;

        size_type align_up(size_type n, size_type align) const noexcept {
            return (n + align - 1) & ~(align - 1);
        }

    public:
        memory_manager() noexcept = default;
        memory_manager(const memory_manager&)            = delete;
        memory_manager& operator=(const memory_manager&) = delete;

        // ──────────── Init ────────────

        /// Инициализировать менеджер памяти.
        ///
        /// Платформа говорит максимумы (platform_max_*), пользователь —
        /// сколько реально использовать. Значения обрезаются до максимумов.
        ///
        /// heap_bytes        — байт под внутреннюю кучу (0 = не использовать).
        /// stack_pool_words   — слов под пул стеков (0 = не использовать).
        /// ext_ram_words      — слов внешней RAM (0 = не использовать).
        /// ext_rom_bytes      — байт внешней ROM (0 = не использовать).
        void init(size_type heap_bytes,
                  size_type stack_pool_words,
                  size_type ext_ram_words = 0,
                  size_type ext_rom_bytes = 0) noexcept {
            // ── Внутренняя куча ──
            size_type max_heap = platform_max_heap_size();
            if (max_heap > 0 && heap_bytes > 0) {
                if (heap_bytes > max_heap) heap_bytes = max_heap;
                heap_size_  = heap_bytes;
                heap_start_ = reinterpret_cast<uint8_t*>(0x20000000);  // ⚠️ platform.cpp
                auto* block = reinterpret_cast<heap_block*>(heap_start_);
                block->size = 0;
                block->next = nullptr;
                heap_free_list_ = block;
            }

            // ── Пул стеков ──
            size_type max_pool = platform_max_stack_pool_size();
            if (max_pool > 0 && stack_pool_words > 0) {
                if (stack_pool_words > max_pool) stack_pool_words = max_pool;
                stack_pool_size_ = stack_pool_words;
                stack_pool_base_ = reinterpret_cast<uint32_t*>(0x20000000 + heap_bytes);  // ⚠️ platform.cpp
                stack_slot_count_ = 0;
                stack_pool_used_  = 0;
            }

            // ── Внешняя RAM ──
            if (platform_has_ext_ram() && ext_ram_words > 0) {
                size_type max_ext = platform_max_ext_ram_size();
                if (ext_ram_words > max_ext) ext_ram_words = max_ext;
                ext_ram_base_ = platform_ext_ram_base();
                if (ext_ram_base_) {
                    ext_ram_size_   = ext_ram_words;
                    ext_ram_active_ = true;
                    // Добавляем внешнюю RAM как свободный блок в кучу
                    auto* block = reinterpret_cast<heap_block*>(ext_ram_base_);
                    block->size = 0;
                    block->next = heap_free_list_;
                    heap_free_list_ = block;
                }
            }

            // ── Внешняя ROM ──
            if (platform_has_ext_rom() && ext_rom_bytes > 0) {
                size_type max_rom = platform_max_ext_rom_size();
                if (ext_rom_bytes > max_rom) ext_rom_bytes = max_rom;
                ext_rom_size_   = ext_rom_bytes;
                ext_rom_active_ = true;
            }
        }

        // ──────────── Stack pool ────────────

        /// Выделить стек из пула. nullptr если не хватает.
        uint32_t* stack_alloc(size_type words) noexcept {
            if (stack_pool_used_ + words > stack_pool_size_) return nullptr;
            if (stack_slot_count_ >= kMaxTrackedTasks) return nullptr;

            uint32_t* base = stack_pool_base_ + stack_pool_used_;
            stack_pool_used_ += words;

            stack_slots_[stack_slot_count_].base  = base;
            stack_slots_[stack_slot_count_].words = words;
            stack_slots_[stack_slot_count_].task  = nullptr;
            ++stack_slot_count_;

            return base;
        }

        /// Вернуть стек в пул.
        void stack_free(uint32_t* stack) noexcept {
            stack_slot* slot = find_stack_slot(stack);
            if (!slot) return;
            slot->task  = nullptr;
            slot->words = 0;
        }

        /// Зарегистрировать задачу и инициализировать стек.
        /// Возвращает SP для первой загрузки.
        uint32_t* task_stack_init(TaskHandler t, uint32_t* stack, size_type words) noexcept {
            if (!t || !stack || words == 0) return nullptr;

            platform_stack_paint(stack, words);

            stack_slot* slot = find_stack_slot(stack);
            if (slot) slot->task = t;

            if (tracked_count_ < kMaxTrackedTasks) {
                tracked_tasks_[tracked_count_].task        = t;
                tracked_tasks_[tracked_count_].stack_base  = stack;
                tracked_tasks_[tracked_count_].stack_words = words;
                tracked_tasks_[tracked_count_].high_water  = words;
                ++tracked_count_;
            }

            uint32_t* sp = stack + words;
            return platform_stack_init_frame(sp, t->fn(), t->ctx());
        }

        /// Снять задачу с учёта.
        void task_unregister(TaskHandler t) noexcept {
            task_mem_info* info = find_tracked(t);
            if (!info) return;

            size_type idx = static_cast<size_type>(info - tracked_tasks_);
            for (size_type i = idx; i + 1 < tracked_count_; ++i) {
                tracked_tasks_[i] = tracked_tasks_[i + 1];
            }
            --tracked_count_;
        }

        // ──────────── Heap ────────────

        /// Выделить из кучи (внутренняя + внешняя RAM). nullptr если не хватает.
        void* heap_alloc(size_type bytes) noexcept {
            if (bytes == 0 || heap_free_list_ == nullptr) return nullptr;

            size_type needed = align_up(bytes + kHeapMinBlock, kHeapAlign);

            heap_block* prev = nullptr;
            heap_block* cur  = heap_free_list_;

            while (cur) {
                size_type avail = cur->size;
                if (avail == 0) {
                    uint8_t* block_end = heap_start_ + heap_size_;
                    uint8_t* cur_ptr   = reinterpret_cast<uint8_t*>(cur);
                    // Блок может быть во внешней RAM — тогда конец другой
                    if (ext_ram_active_ && cur_ptr >= reinterpret_cast<uint8_t*>(ext_ram_base_)) {
                        block_end = reinterpret_cast<uint8_t*>(ext_ram_base_ + ext_ram_size_);
                    }
                    avail = static_cast<size_type>(block_end - cur_ptr);
                }

                if (avail >= needed) {
                    if (avail >= needed + kHeapMinBlock + kHeapAlign) {
                        auto* new_block = reinterpret_cast<heap_block*>(
                            reinterpret_cast<uint8_t*>(cur) + needed);
                        new_block->size = 0;
                        new_block->next = cur->next;
                        cur->size  = needed;
                        cur->next  = nullptr;

                        if (prev) prev->next = new_block;
                        else      heap_free_list_ = new_block;
                    } else {
                        cur->size = needed;
                        if (prev) prev->next = cur->next;
                        else      heap_free_list_ = cur->next;
                    }
                    return reinterpret_cast<uint8_t*>(cur) + kHeapMinBlock;
                }

                prev = cur;
                cur  = cur->next;
            }

            return nullptr;
        }

        /// Вернуть в кучу.
        void heap_free(void* ptr) noexcept {
            if (!ptr) return;

            auto* block = reinterpret_cast<heap_block*>(
                static_cast<uint8_t*>(ptr) - kHeapMinBlock);
            block->size = 0;
            block->next = heap_free_list_;
            heap_free_list_ = block;
        }

        // ──────────── Monitoring ────────────
        /// Свободных слов в стеке задачи (сейчас).
        size_type task_stack_free(TaskHandler t) noexcept {
            task_mem_info* info = find_tracked(t);
            if (!info) return 0;

            size_type free = 0;
            for (size_type i = 0; i < info->stack_words; ++i) {
                if (info->stack_base[i] != kStackFill) break;
                ++free;
            }
            return free;
        }

        /// Минимум свободных слов с момента регистрации.
        size_type task_stack_high_water(TaskHandler t) noexcept {
            task_mem_info* info = find_tracked(t);
            return info ? info->high_water : 0;
        }

        /// Вышла ли задача за пределы стека.
        bool task_stack_overflow(TaskHandler t) noexcept {
            task_mem_info* info = find_tracked(t);
            if (!info) return false;
            return info->stack_base[0] != kStackMagic;
        }

        /// Обновить high water mark для задачи.
        void task_update_high_water(TaskHandler t) noexcept {
            task_mem_info* info = find_tracked(t);
            if (!info) return;
            size_type free = task_stack_free(t);
            if (free < info->high_water) {
                info->high_water = free;
            }
        }

        /// Обновить high water mark для всех задач.
        void update_all_high_water() noexcept {
            for (size_type i = 0; i < tracked_count_; ++i) {
                task_update_high_water(tracked_tasks_[i].task);
            }
        }

        // ──────────── Defragmentation ────────────

        /// Дефрагментировать кучу — слить смежные свободные блоки.
        void heap_defrag() noexcept {
            heap_merge_free();
        }

        /// Дефрагментировать пул стеков — сжать свободное место.
        void stack_pool_defrag() noexcept {
            size_type write_pos = 0;

            for (size_type i = 0; i < stack_slot_count_; ++i) {
                stack_slot& slot = stack_slots_[i];

                if (slot.words == 0) continue;

                if (slot.task) {
                    task_state st = slot.task->state();
                    if (st == task_state::suspended ||
                        st == task_state::idle ||
                        st == task_state::done) {
                        if (slot.base != stack_pool_base_ + write_pos) {
                            uint32_t* dst = stack_pool_base_ + write_pos;
                            for (size_type w = 0; w < slot.words; ++w) {
                                dst[w] = slot.base[w];
                            }
                            slot.base = dst;
                        }
                        write_pos += slot.words;
                    } else {
                        write_pos = static_cast<size_type>(
                            slot.base - stack_pool_base_ + slot.words);
                    }
                }
            }

            stack_pool_used_ = write_pos;

            size_type j = 0;
            for (size_type i = 0; i < stack_slot_count_; ++i) {
                if (stack_slots_[i].words > 0) {
                    if (i != j) stack_slots_[j] = stack_slots_[i];
                    ++j;
                }
            }
            stack_slot_count_ = j;
        }

        /// Полная дефрагментация.
        void defrag() noexcept {
            heap_defrag();
            stack_pool_defrag();
        }

        // ──────────── External ROM (файловое хранилище) ────────────

        bool ext_rom_active() const noexcept { return ext_rom_active_; }

        /// Прочитать данные из внешней ROM.
        bool ext_rom_read(uint32_t offset, uint8_t* buf, uint32_t len) noexcept {
            if (!ext_rom_active_) return false;
            if (offset + len > ext_rom_size_) return false;
            return platform_ext_rom_read(offset, buf, len);
        }

        /// Записать данные во внешнюю ROM.
        bool ext_rom_write(uint32_t offset, const uint8_t* buf, uint32_t len) noexcept {
            if (!ext_rom_active_) return false;
            if (offset + len > ext_rom_size_) return false;
            return platform_ext_rom_write(offset, buf, len);
        }

        /// Стереть сектор внешней ROM.
        bool ext_rom_erase(uint32_t offset, uint32_t len) noexcept {
            if (!ext_rom_active_) return false;
            if (offset + len > ext_rom_size_) return false;
            return platform_ext_rom_erase(offset, len);
        }

        // ──────────── Stats ────────────

        size_type heap_total()      const noexcept { return heap_size_; }
        size_type heap_free_bytes() const noexcept {
            size_type free = 0;
            heap_block* cur = heap_free_list_;
            while (cur) {
                if (cur->size == 0) {
                    uint8_t* block_end = heap_start_ + heap_size_;
                    uint8_t* cur_ptr   = reinterpret_cast<uint8_t*>(cur);
                    if (ext_ram_active_ && cur_ptr >= reinterpret_cast<uint8_t*>(ext_ram_base_)) {
                        block_end = reinterpret_cast<uint8_t*>(ext_ram_base_ + ext_ram_size_);
                    }
                    free += static_cast<size_type>(block_end - cur_ptr);
                }
                cur = cur->next;
            }
            return free;
        }
        size_type heap_used() const noexcept { return heap_size_ - heap_free_bytes(); }

        size_type stack_pool_total() const noexcept { return stack_pool_size_; }
        size_type stack_pool_used()  const noexcept { return stack_pool_used_; }
        size_type stack_pool_free()  const noexcept { return stack_pool_size_ - stack_pool_used_; }

        size_type ext_ram_total()    const noexcept { return ext_ram_size_; }
        bool      ext_ram_active()   const noexcept { return ext_ram_active_; }

        size_type ext_rom_total()    const noexcept { return ext_rom_size_; }

        size_type tracked_count()    const noexcept { return tracked_count_; }

    private:
        static constexpr uint32_t kStackMagic = 0xDEADBEEF;
        static constexpr uint32_t kStackFill  = 0xA5A5A5A5;
    };

    // ────────────────────────────────────────────────────────────
    // Inline helpers
    // ────────────────────────────────────────────────────────────

    inline memory_manager::task_mem_info*
    memory_manager::find_tracked(TaskHandler t) noexcept {
        for (size_type i = 0; i < tracked_count_; ++i) {
            if (tracked_tasks_[i].task == t) return &tracked_tasks_[i];
        }
        return nullptr;
    }

    inline memory_manager::stack_slot*
    memory_manager::find_stack_slot(uint32_t* base) noexcept {
        for (size_type i = 0; i < stack_slot_count_; ++i) {
            if (stack_slots_[i].base == base) return &stack_slots_[i];
        }
        return nullptr;
    }

    inline void memory_manager::heap_merge_free() noexcept {
        if (!heap_free_list_) return;

        bool swapped;
        do {
            swapped = false;
            heap_block* prev = nullptr;
            heap_block* cur  = heap_free_list_;
            while (cur && cur->next) {
                if (cur > cur->next) {
                    heap_block* a = cur;
                    heap_block* b = cur->next;
                    a->next = b->next;
                    b->next = a;
                    if (prev) prev->next = b;
                    else      heap_free_list_ = b;
                    cur = b;
                    swapped = true;
                }
                prev = cur;
                cur  = cur->next;
            }
        } while (swapped);

        heap_block* cur = heap_free_list_;
        while (cur && cur->next) {
            uint8_t* cur_end = reinterpret_cast<uint8_t*>(cur) +
                (cur->size == 0 ? kHeapMinBlock : cur->size);
            uint8_t* next_ptr = reinterpret_cast<uint8_t*>(cur->next);

            if (cur_end == next_ptr) {
                cur->size = 0;
                cur->next = cur->next->next;
            } else {
                cur = cur->next;
            }
        }
    }

} // namespace embed
