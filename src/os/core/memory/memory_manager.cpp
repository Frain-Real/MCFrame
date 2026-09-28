#include "memory_manager.hpp"

inline embed::memory_manager::task_mem_info* embed::memory_manager::find_tracked(TaskHandler t) noexcept {
    for (Size i = 0; i < tracked_count_; ++i) {
        if (tracked_tasks_[i].task == t) return &tracked_tasks_[i];
    }
    return nullptr;
}

inline embed::memory_manager::stack_slot* embed::memory_manager::find_stack_slot(uint32_t* base) noexcept {
    for (Size i = 0; i < stack_slot_count_; ++i) {
        if (stack_slots_[i].base == base) return &stack_slots_[i];
    }
    return nullptr;
}

inline void embed::memory_manager::heap_merge_free() noexcept {
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

bool embed::memory_manager::ext_rom_active() const noexcept { return ext_rom_active_; }

bool embed::memory_manager::ext_rom_read(uint32_t offset, uint8_t* buf, uint32_t len) noexcept {
    if (!ext_rom_active_) return false;
    if (offset + len > ext_rom_size_) return false;
    return platform_ext_rom_read(offset, buf, len);
}

bool embed::memory_manager::ext_rom_write(uint32_t offset, const uint8_t* buf, uint32_t len) noexcept {
    if (!ext_rom_active_) return false;
    if (offset + len > ext_rom_size_) return false;
    return platform_ext_rom_write(offset, buf, len);
}

bool embed::memory_manager::ext_rom_erase(uint32_t offset, uint32_t len) noexcept {
    if (!ext_rom_active_) return false;
    if (offset + len > ext_rom_size_) return false;
    return platform_ext_rom_erase(offset, len);
}

void embed::memory_manager::heap_defrag() noexcept {
    heap_merge_free();
}

void embed::memory_manager::stack_pool_defrag() noexcept {
    Size write_pos = 0;
    for (Size i = 0; i < stack_slot_count_; ++i) {
        stack_slot& slot = stack_slots_[i];
        if (slot.words == 0) continue;
        if (slot.task) {
            task_state st = slot.task->state();
            if (st == task_state::suspended ||
                st == task_state::idle ||
                st == task_state::done) {
                if (slot.base != stack_pool_base_ + write_pos) {
                    uint32_t* dst = stack_pool_base_ + write_pos;
                    for (Size w = 0; w < slot.words; ++w) {
                        dst[w] = slot.base[w];
                    }
                    slot.base = dst;
                }
                write_pos += slot.words;
            } else {
                write_pos = static_cast<Size>(
                    slot.base - stack_pool_base_ + slot.words);
            }
        }
    }

    stack_pool_used_ = write_pos;

    Size j = 0;
    for (Size i = 0; i < stack_slot_count_; ++i) {
        if (stack_slots_[i].words > 0) {
            if (i != j) stack_slots_[j] = stack_slots_[i];
            ++j;
        }
    }
    stack_slot_count_ = j;
}
void embed::memory_manager::defrag() noexcept {
    heap_defrag();
    stack_pool_defrag();
}


embed::Size embed::memory_manager::task_stack_free(TaskHandler t) noexcept {
    task_mem_info* info = find_tracked(t);
    if (!info) return 0;

    Size free_mem = 0;
    for (Size i = 0; i < info->stack_words; ++i) {
        if (info->stack_base[i] != kStackFill) break;
        ++free_mem;
    }
    return free_mem;
}
embed::Size embed::memory_manager::task_stack_high_water(TaskHandler t) noexcept {
    task_mem_info* info = find_tracked(t);
    return info ? info->high_water : 0;
}
bool embed::memory_manager::task_stack_overflow(TaskHandler t) noexcept {
    task_mem_info* info = find_tracked(t);
    if (!info) return false;
    return info->stack_base[0] != kStackMagic;
}
void embed::memory_manager::task_update_high_water(TaskHandler t) noexcept {
    task_mem_info* info = find_tracked(t);
    if (!info) return;
    Size free_mem = task_stack_free(t);
    if (free_mem < info->high_water) {
        info->high_water = free_mem;
    }
}
void embed::memory_manager::update_all_high_water() noexcept {
    for (Size i = 0; i < tracked_count_; ++i) {
        task_update_high_water(tracked_tasks_[i].task);
    }
}

// ──────────── Heap ────────────
/// Выделить из кучи (внутренняя + внешняя RAM). nullptr если не хватает.
void* embed::memory_manager::heap_alloc(Size bytes) noexcept {
    if (bytes == 0 || heap_free_list_ == nullptr) return nullptr;

    Size needed = align_up(bytes + kHeapMinBlock, kHeapAlign);

    heap_block* prev = nullptr;
    heap_block* cur  = heap_free_list_;

    while (cur) {
        Size avail = cur->size;
        if (avail == 0) {
            uint8_t* block_end = heap_start_ + heap_size_;
            uint8_t* cur_ptr   = reinterpret_cast<uint8_t*>(cur);

            if (ext_ram_active_ && cur_ptr >= reinterpret_cast<uint8_t*>(ext_ram_base_)) {
                block_end = reinterpret_cast<uint8_t*>(ext_ram_base_ + ext_ram_size_);
            }
            avail = static_cast<Size>(block_end - cur_ptr);
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
void embed::memory_manager::heap_free(void* ptr) noexcept {
    if (!ptr) return;

    auto* block = reinterpret_cast<heap_block*>(
        static_cast<uint8_t*>(ptr) - kHeapMinBlock);
    block->size = 0;
    block->next = heap_free_list_;
    heap_free_list_ = block;
}

uint32_t* embed::memory_manager::task_stack_init(TaskHandler t, uint32_t* stack, Size words) noexcept {
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
    sp = platform_stack_init_frame(sp, t->fn(), t->ctx());
    t->save_sp(sp);
    return sp;
}

/// Снять задачу с учёта.
void embed::memory_manager::task_unregister(TaskHandler t) noexcept {
    task_mem_info* info = find_tracked(t);
    if (!info) return;

    Size idx = static_cast<Size>(info - tracked_tasks_);
    for (Size i = idx; i + 1 < tracked_count_; ++i) {
        tracked_tasks_[i] = tracked_tasks_[i + 1];
    }
    --tracked_count_;
}

uint32_t* embed::memory_manager::stack_alloc(Size words) noexcept {
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

// Вернуть стек в пул.
void embed::memory_manager::stack_free(uint32_t* stack) noexcept {
    stack_slot* slot = find_stack_slot(stack);
    if (!slot) return;
    slot->task  = nullptr;
    slot->words = 0;
}

void embed::memory_manager::init(Size heap_bytes, Size stack_pool_words, Size ext_ram_words, Size ext_rom_bytes) noexcept {
    Size max_heap = platform_max_heap_size();
    if (max_heap > 0 && heap_bytes > 0) {
        if (heap_bytes > max_heap) heap_bytes = max_heap;
        heap_size_  = heap_bytes;
        heap_start_ = reinterpret_cast<uint8_t*>(0x20000000);
        auto* block = reinterpret_cast<heap_block*>(heap_start_);
        block->size = 0;
        block->next = nullptr;
        heap_free_list_ = block;
    }
    Size max_pool = platform_max_stack_pool_size();
    if (max_pool > 0 && stack_pool_words > 0) {
        if (stack_pool_words > max_pool) stack_pool_words = max_pool;
        stack_pool_size_ = stack_pool_words;
        stack_pool_base_ = reinterpret_cast<uint32_t*>(0x20000000 + heap_bytes);
        stack_slot_count_ = 0;
        stack_pool_used_  = 0;
    }
    if (platform_has_ext_ram() && ext_ram_words > 0) {
        Size max_ext = platform_max_ext_ram_size();
        if (ext_ram_words > max_ext) ext_ram_words = max_ext;
        ext_ram_base_ = platform_ext_ram_base();
        if (ext_ram_base_) {
            ext_ram_size_   = ext_ram_words;
            ext_ram_active_ = true;
            auto* block = reinterpret_cast<heap_block*>(ext_ram_base_);
            block->size = 0;
            block->next = heap_free_list_;
            heap_free_list_ = block;
        }
    }
    if (platform_has_ext_rom() && ext_rom_bytes > 0) {
        Size max_rom = platform_max_ext_rom_size();
        if (ext_rom_bytes > max_rom) ext_rom_bytes = max_rom;
        ext_rom_size_   = ext_rom_bytes;
        ext_rom_active_ = true;
    }
}