// memory_manager.cpp
#include "memory_manager.hpp"

embed::memory_manager::task_slot*
embed::memory_manager::find_slot(TaskHandler t) noexcept {
    if (t == nullptr) return nullptr;
    for (Size i = 0; i < kMaxTrackedTasks; ++i) {
        if (slots_[i].task == t) return &slots_[i];
    }
    return nullptr;
}

embed::memory_manager::task_slot*
embed::memory_manager::find_slot_by_stack(uint32_t* base) noexcept {
    if (base == nullptr) return nullptr;
    for (Size i = 0; i < kMaxTrackedTasks; ++i) {
        if (slots_[i].task != nullptr && slots_[i].stack_base == base) return &slots_[i];
    }
    return nullptr;
}

embed::Size embed::memory_manager::tracked_count() const noexcept {
    Size n = 0u;
    for (Size i = 0; i < kMaxTrackedTasks; ++i) {
        if (slots_[i].task != nullptr) ++n;
    }
    return n;
}

// Слияние блока b со следующим по адресу, если тот свободен.
void embed::memory_manager::heap_coalesce(heap_block* b) noexcept {
    if (!b || !b->is_free()) return;

    const auto addr = [](const heap_block* x) noexcept {
        return reinterpret_cast<uintptr_t>(x);
    };

    // Следующий по адресу блок — только если он внутри кучи.
    uint8_t* end = heap_start_ + heap_size_;
    uintptr_t b_next = addr(b) + b->real_size();
    if (b_next >= reinterpret_cast<uintptr_t>(end)) return;

    auto* nb = reinterpret_cast<heap_block*>(b_next);
    if (nb->magic != kHeapMagic || !nb->is_free()) return;

    // Забираем соседа: удаляем его из free-list, растим текущий.
    heap_block** pp = &heap_free_list_;
    while (*pp && *pp != nb) pp = &(*pp)->next;
    if (*pp == nb) *pp = nb->next;

    b->set_size(b->real_size() + nb->real_size(), true);
}

void* embed::memory_manager::heap_alloc(Size bytes) noexcept {
    if (!initialized_ || bytes == 0u) return nullptr;

    // Заголовок + выравнивание, с защитой от переполнения.
    if (bytes > 0xFFFFFFFFu - kHeapMinBlock - kHeapAlign) return nullptr;

    const Size needed = align_up(bytes + kHeapMinBlock, kHeapAlign);

    heap_block* prev = nullptr;
    heap_block* cur  = heap_free_list_;

    while (cur) {
        const Size avail = cur->real_size();

        if (avail >= needed) {
            const Size rest = avail - needed;

            if (rest >= kHeapMinBlock + kHeapAlign) {
                // Расщепляем: [выделенный needed][новый свободный rest].
                // Свободного стало меньше ровно на needed: остаток rest
                // остаётся в куче как отдельный блок.
                auto* nb = reinterpret_cast<heap_block*>(
                    reinterpret_cast<uint8_t*>(cur) + needed);
                nb->set_size(rest, true);
                nb->magic = kHeapMagic;
                nb->next  = cur->next;

                cur->set_size(needed, false);
                cur->next = nullptr;

                if (prev) prev->next = nb;
                else      heap_free_list_ = nb;

                heap_free_ -= needed;
            } else {
                // Отдаём блок целиком: остаток слишком мал для заголовка,
                // поэтому он уходит пользователю вместе с блоком.
                cur->set_size(avail, false);
                if (prev) prev->next = cur->next;
                else      heap_free_list_ = cur->next;
                cur->next = nullptr;

                heap_free_ -= avail;
            }

            return reinterpret_cast<uint8_t*>(cur) + kHeapMinBlock;
        }

        prev = cur;
        cur  = cur->next;
    }

    return nullptr;
}

void embed::memory_manager::heap_free(void* ptr) noexcept {
    if (!ptr || !initialized_) return;

    // Проверяем, что указатель вообще принадлежит нашей куче.
    auto* raw = static_cast<uint8_t*>(ptr);
    if (raw < heap_start_ + kHeapMinBlock ||
        raw >= heap_start_ + heap_size_) {
        ++heap_faults_;
        return;
    }

    auto* b = reinterpret_cast<heap_block*>(raw - kHeapMinBlock);
    if (b->magic != kHeapMagic) {          // чужой указатель или порча
        ++heap_faults_;
        return;
    }
    if (b->is_free()) {                    // двойное освобождение
        ++heap_faults_;
        return;
    }

    const Size n = b->real_size();
    b->set_size(n, true);
    heap_free_ += n;

    // Вставляем в free-list в порядке адресов, затем сливаем с соседями.
    heap_block** pp = &heap_free_list_;
    while (*pp && *pp < b) pp = &(*pp)->next;
    b->next = *pp;
    *pp = b;

    heap_coalesce(b);
}

embed::Size embed::memory_manager::heap_largest_free() const noexcept {
    Size best = 0;
    for (const heap_block* cur = heap_free_list_; cur; cur = cur->next) {
        if (cur->real_size() > best) best = cur->real_size();
    }
    return best;
}

bool embed::memory_manager::heap_check() const noexcept {
    if (!initialized_) return false;

    const uint8_t* begin = heap_start_;
    const uint8_t* end   = heap_start_ + heap_size_;

    Size counted_free = 0;
    const uint8_t* p = begin;
    while (p < end) {
        const auto* b = reinterpret_cast<const heap_block*>(p);
        if (b->magic != kHeapMagic) return false;
        const Size sz = b->real_size();
        if (sz < kHeapMinBlock || sz % kHeapAlign != 0u) return false;
        if (p + sz > end) return false;
        if (b->is_free()) counted_free += sz;
        p += sz;
    }
    if (p != end) return false;
    return counted_free == heap_free_;
}

embed::Size embed::memory_manager::task_stack_free(TaskHandler t) noexcept {
    task_slot* info = find_slot(t);
    if (!info) return 0;

    // Стек растёт ВНИЗ: идём от вершины к дну и считаем нетронутые слова.
    constexpr uint32_t kFill = 0xA5A5A5A5u;
    Size i = info->stack_words;
    while (i > 0u && info->stack_base[i - 1u] == kFill) --i;
    return i;
}

embed::Size embed::memory_manager::task_stack_high_water(TaskHandler t) noexcept {
    task_slot* info = find_slot(t);
    return info ? info->high_water : 0u;
}

bool embed::memory_manager::task_stack_overflow(TaskHandler t) noexcept {
    task_slot* info = find_slot(t);
    if (!info || info->stack_base == nullptr) return false;
    // Дно стека (младший адрес) хранит магическое значение; если оно затёрто —
    // стек дорос до самого низа.
    return info->stack_base[0] != 0xDEADBEEFu;
}

void embed::memory_manager::task_update_high_water(TaskHandler t) noexcept {
    task_slot* info = find_slot(t);
    if (!info || info->stack_base == nullptr || info->stack_words == 0u) return;
    const Size free_words = task_stack_free(t);
    if (free_words < info->high_water) info->high_water = free_words;
}

void embed::memory_manager::task_reset_high_water(TaskHandler t) noexcept {
    task_slot* info = find_slot(t);
    if (info == nullptr) return;
    info->high_water = info->stack_words;
}

void embed::memory_manager::reset_all_high_water() noexcept {
    for (Size i = 0; i < kMaxTrackedTasks; ++i) {
        slots_[i].high_water = slots_[i].stack_words;
    }
}

void embed::memory_manager::update_all_high_water() noexcept {
    for (Size i = 0; i < kMaxTrackedTasks; ++i) {
        if (slots_[i].task != nullptr) task_update_high_water(slots_[i].task);
    }
}

// ──────────── External ROM ────────────

bool embed::memory_manager::ext_rom_read(uint32_t offset, uint8_t* buf, uint32_t len) noexcept {
    if (!ext_rom_active_ || !buf) return false;
    if (offset > ext_rom_size_ || len > ext_rom_size_ - offset) return false;
    return platform::ops().ext_rom_read(offset, buf, len);
}

bool embed::memory_manager::ext_rom_write(uint32_t offset, const uint8_t* buf, uint32_t len) noexcept {
    if (!ext_rom_active_ || !buf) return false;
    if (offset > ext_rom_size_ || len > ext_rom_size_ - offset) return false;
    return platform::ops().ext_rom_write(offset, buf, len);
}

bool embed::memory_manager::ext_rom_erase(uint32_t offset, uint32_t len) noexcept {
    if (!ext_rom_active_) return false;
    if (offset > ext_rom_size_ || len > ext_rom_size_ - offset) return false;
    return platform::ops().ext_rom_erase(offset, len);
}

// ──────────── Stack pool ────────────

/// Настроить стек задачи: покрасить, зарегистрировать владельца слота,
/// поставить на учёт и собрать стартовый фрейм для первой загрузки контекста.
uint32_t* embed::memory_manager::task_stack_init(TaskHandler t, uint32_t* stack, Size words) noexcept {
    if (!t || !stack || words == 0u) return nullptr;

    const platform::Ops& p = platform::ops();
    if (p.stack_paint) p.stack_paint(stack, static_cast<uint32_t>(words));

    // Переиспользуем запись, если задача уже учтена; иначе ищем свободный слот.
    task_slot* info = find_slot(t);
    if (info == nullptr) {
        for (Size i = 0; i < kMaxTrackedTasks; ++i) {
            if (slots_[i].task == nullptr) { info = &slots_[i]; break; }
        }
    }
    if (info == nullptr) return nullptr;      // таблица заполнена

    info->task        = t;
    info->stack_base  = stack;
    info->stack_words = words;
    info->high_water  = words;

    uint32_t* sp = stack + words;
    if (p.stack_init_frame) sp = p.stack_init_frame(sp, t->fn(), t->ctx());
    if (sp == nullptr) return nullptr;

    t->save_sp(sp);
    return sp;
}
void embed::memory_manager::task_unregister(TaskHandler t) noexcept {
    task_slot* slot = find_slot(t);
    if (slot == nullptr) return;
    // Слот освобождается, но занимаемое им место в пуле НЕ возвращается:
    // стек выделял вызывающий (обычно статический массив).
    *slot = task_slot{};
}

uint32_t* embed::memory_manager::stack_alloc(Size words) noexcept {
    if (!initialized_ || words == 0u || stack_pool_size_ == 0u) return nullptr;

    // 1. Переиспользуем освобождённый слот ТОГО ЖЕ размера: иначе пул
    //    исчерпывался после нескольких циклов выделить/освободить.
    for (Size i = 0; i < kMaxTrackedTasks; ++i) {
        if (slots_[i].task == nullptr &&
            slots_[i].stack_base != nullptr &&
            slots_[i].stack_words == words) {
            slots_[i].high_water = words;
            return slots_[i].stack_base;
        }
    }

    if (stack_pool_used_ + words > stack_pool_size_) return nullptr;

    // 2. Новый участок пула.
    uint32_t* base = stack_pool_base_ + stack_pool_used_;
    stack_pool_used_ += words;

    for (Size i = 0; i < kMaxTrackedTasks; ++i) {
        if (slots_[i].task == nullptr && slots_[i].stack_base == nullptr) {
            slots_[i].stack_base  = base;
            slots_[i].stack_words = words;
            slots_[i].high_water  = words;
            return base;
        }
    }

    // Свободных записей нет — откатываем выделение из пула.
    stack_pool_used_ -= words;
    return nullptr;
}

void embed::memory_manager::stack_free(uint32_t* stack) noexcept {
    task_slot* slot = find_slot_by_stack(stack);
    if (slot == nullptr) return;

    // Возвращаем слова пулу и сохраняем участок для переиспользования:
    // stack_base остаётся, task обнуляется (см. stack_alloc, шаг 1).
    if (slot->task != nullptr) {
        if (stack_pool_used_ >= slot->stack_words) {
            stack_pool_used_ -= slot->stack_words;
        }
    }
    slot->task       = nullptr;
    slot->high_water = slot->stack_words;
}

// ──────────── Init ────────────

embed::Status embed::memory_manager::init(Size heap_bytes, Size stack_pool_words,
                                          Size ext_ram_words, Size ext_rom_bytes) noexcept {
    if (initialized_) return Status::AlreadyExists;

    for (Size i = 0; i < kMaxTrackedTasks; ++i) slots_[i] = task_slot{};

    const platform::Ops& p = platform::ops();

    // ── Heap ──
    uint8_t* heap_base = p.heap_base ? p.heap_base() : nullptr;
    Size heap_cap       = p.heap_capacity ? p.heap_capacity() : 0u;

    if (heap_base != nullptr && heap_cap > 0u && heap_bytes > 0u) {
        if (heap_bytes > heap_cap) heap_bytes = heap_cap;
        heap_bytes = align_up(heap_bytes, kHeapAlign);
        if (heap_bytes > heap_cap) heap_bytes = heap_cap & ~(kHeapAlign - 1u);

        if (heap_bytes >= kHeapMinBlock * 2u) {
            heap_start_ = heap_base;
            heap_size_  = heap_bytes;

            auto* first = reinterpret_cast<heap_block*>(heap_start_);
            first->set_size(heap_bytes, true);
            first->magic = kHeapMagic;
            first->next  = nullptr;

            heap_first_     = first;
            heap_free_list_ = first;
            heap_free_      = heap_bytes;
        }
    }

    // ── Stack pool ──
    uint32_t* pool_base = p.stack_pool_base ? p.stack_pool_base() : nullptr;
    Size pool_cap       = p.stack_pool_capacity ? p.stack_pool_capacity() : 0u;

    if (pool_base != nullptr && pool_cap > 0u && stack_pool_words > 0u) {
        if (stack_pool_words > pool_cap) stack_pool_words = pool_cap;
        stack_pool_base_ = pool_base;
        stack_pool_size_ = stack_pool_words;
        stack_pool_used_ = 0u;
    }

    // ── External RAM ──
    if (p.has_ext_ram && p.has_ext_ram() && ext_ram_words > 0u) {
        Size cap = p.ext_ram_capacity ? p.ext_ram_capacity() : 0u;
        if (ext_ram_words > cap) ext_ram_words = cap;
        uint32_t* base = p.ext_ram_base ? p.ext_ram_base() : nullptr;
        if (base != nullptr && ext_ram_words > 0u) {
            ext_ram_base_   = base;
            ext_ram_size_   = ext_ram_words;
            ext_ram_active_ = true;
        }
    }

    // ── External ROM ──
    if (p.has_ext_rom && p.has_ext_rom() && ext_rom_bytes > 0u) {
        Size cap = p.ext_rom_capacity ? p.ext_rom_capacity() : 0u;
        if (ext_rom_bytes > cap) ext_rom_bytes = cap;
        if (ext_rom_bytes > 0u) {
            ext_rom_size_   = ext_rom_bytes;
            ext_rom_active_ = true;
        }
    }

    initialized_ = true;
    return Status::Ok;
}
