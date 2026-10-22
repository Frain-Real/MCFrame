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
        // task МОЖЕТ быть nullptr: регион, выданный stack_alloc(), стоит на
        // учёте ДО того, как его отдали задаче, и остаётся на учёте после
        // task_unregister() — иначе возвращать его в пул будет не по чему.
        // Прежняя проверка «task != nullptr» делала такие регионы
        // невидимыми для stack_free(): слова пула утекали навсегда.
        if (slots_[i].stack_base == base) return &slots_[i];
    }
    return nullptr;
}

embed::memory_manager::task_slot*
embed::memory_manager::find_free_slot() noexcept {
    for (Size i = 0; i < kMaxTrackedTasks; ++i) {
        if (slots_[i].task == nullptr && slots_[i].stack_base == nullptr) {
            return &slots_[i];
        }
    }
    return nullptr;
}

bool embed::memory_manager::is_pool_region(const uint32_t* base, Size words) const noexcept {
    if (stack_pool_size_ == 0u || base == nullptr || words == 0u) return false;
    const uintptr_t lo = reinterpret_cast<uintptr_t>(stack_pool_base_);
    const uintptr_t hi = lo + stack_pool_size_ * sizeof(uint32_t);
    const uintptr_t b  = reinterpret_cast<uintptr_t>(base);
    return b >= lo && (b + words * sizeof(uint32_t)) <= hi;
}

embed::Size embed::memory_manager::tracked_count() const noexcept {
    Size n = 0u;
    for (Size i = 0; i < kMaxTrackedTasks; ++i) {
        if (slots_[i].task != nullptr) ++n;
    }
    return n;
}

// Слияние освобождённого блока с физическими соседями — в ОБЕ стороны.
//
// Прежде сливалось только ВПЕРЁД, и этого мало. Сценарий: A, B, C идут
// подряд; освобождаем A, затем C, затем B. Блок B сливается вперёд — в списке
// остаются ДВА СОСЕДНИХ свободных блока: A и (B+C). Они уже никогда не
// сольются, потому что слияние запускается только при free() и только для
// освобождаемого блока. heap_free_ при этом сходится (байты не теряются),
// поэтому heap_check() проходил, но first-fit не видел сплошного куска, а
// heap_largest_free() занижал отчёт: «свободный блок 96 Б» в пустой куче.
//
// Назад блок ищется по free-list, а не «по адресам»: единственная запись о
// занятости блока — его собственный заголовок, а адреса начала блока у
// занятого предшественника неизвестен. Список свободных отсортирован по
// адресам (так его поддерживает heap_free), поэтому физический сосед слева —
// это РОВНО предыдущий элемент списка; искать в другом месте нечего.
//
// Контракт: b УЖЕ вставлен в free-list (единственный вызов — из heap_free).
void embed::memory_manager::heap_coalesce(heap_block* b) noexcept {
    if (!b || !b->is_free()) return;

    const auto addr = [](const heap_block* x) noexcept {
        return reinterpret_cast<uintptr_t>(x);
    };

    // ── Шаг 1: слияние НАЗАД ──
    heap_block* prev = nullptr;
    for (heap_block* it = heap_free_list_; it != nullptr; it = it->next) {
        if (addr(it) >= addr(b)) break;
        prev = it;
    }
    if (prev != nullptr && addr(prev) + prev->real_size() == addr(b)) {
        // Поглощаем b левым соседом: он уже стоит в списке ПЕРЕД b, поэтому
        // достаточно выкинуть b из списка и увеличить левый блок.
        prev->set_size(prev->real_size() + b->real_size(), true);
        prev->next = b->next;
        b = prev;                       // дальше сливаем уже объединённый блок
    }

    // ── Шаг 2: слияние ВПЕРЁД ──
    // Следующий по адресу блок — только если он внутри кучи.
    uint8_t* end = heap_start_ + heap_size_;
    const uintptr_t b_next = addr(b) + b->real_size();
    if (b_next >= reinterpret_cast<uintptr_t>(end)) return;

    // Первый свободный ВЫШЕ по адресу — это b->next (инвариант порядка
    // списка). Если он примыкает вплотную — сливаем; если между ними занятый
    // блок, то nb стоит дальше по адресу и слияния нет.
    heap_block* nb = b->next;
    if (nb == nullptr || addr(nb) != b_next) return;
    if (nb->magic != kHeapMagic) return;   // список испорчен — вслепую не сливаем

    // Сосед внутри объединённого блока: из списка убираем, размер забираем.
    b->set_size(b->real_size() + nb->real_size(), true);
    b->next = nb->next;
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

    // Обход по адресам идёт в возрастающем порядке, а free-list обязан быть
    // отсортирован так же (см. heap_free). Сверяем список с обходом на ходу:
    // на инварианте «предыдущий элемент списка = физический сосед слева»
    // держится слияние НАЗАД в heap_coalesce(). Так ловятся и выпавший из
    // списка блок, и дубликат, и потерянный порядок — то есть именно та
    // порча, из-за которой куча позже «внезапно кончается».
    const heap_block* fl = heap_free_list_;

    while (p < end) {
        const auto* b = reinterpret_cast<const heap_block*>(p);
        if (b->magic != kHeapMagic) return false;
        const Size sz = b->real_size();
        if (sz < kHeapMinBlock || sz % kHeapAlign != 0u) return false;
        if (p + sz > end) return false;
        if (b->is_free()) {
            counted_free += sz;
            if (fl != b) return false;      // блок не в списке / порядок нарушен
            fl = fl->next;
        }
        p += sz;
    }
    if (p != end) return false;
    if (fl != nullptr) return false;        // в списке остались блоки вне кучи
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
    return info->stack_base[0] != kStackGuardMagic;
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

    // Кому принадлежит запись учёта, в порядке приоритета:
    //   1) эта задача уже учтена — повторный init;
    //   2) по БАЗЕ стека — так регион от stack_alloc() привязывается именно к
    //      своей записи. Прежде слот искался только по задаче, а иначе брался
    //      первый свободный: регион регистрировался ВТОРОЙ записью, первая
    //      оставалась «сиротой» с той же базой, и учёт разъезжался;
    //   3) полностью свободная запись — для статического массива.
    task_slot* info = find_slot(t);
    if (info != nullptr) {
        // Регион пула нельзя потерять при перепривязке задачи к другому стеку:
        // его сперва возвращают в пул через stack_free().
        if (is_pool_region(info->stack_base, info->stack_words) &&
            info->stack_base != stack) {
            return nullptr;
        }
    } else {
        info = find_slot_by_stack(stack);
        if (info != nullptr) {
            if (info->task != nullptr) return nullptr;       // база занята чужой задачей
            if (info->stack_words != words) return nullptr;  // размер не тот, что у региона
        } else {
            info = find_free_slot();
            if (info == nullptr) return nullptr;             // таблица учёта заполнена
        }
    }

    const platform::Ops& p = platform::ops();
    if (p.stack_paint) p.stack_paint(stack, static_cast<uint32_t>(words));

    info->task        = t;
    info->stack_base  = stack;
    info->stack_words = words;
    info->high_water  = words;
    t->set_stack_base(stack);

    uint32_t* sp = stack + words;
    if (p.stack_init_frame) sp = p.stack_init_frame(sp, t->fn(), t->ctx());
    if (sp == nullptr) {
        // Кадр не собрался: задача не сможет запуститься, поэтому привязку
        // снимаем — «призрака» в учёте остаться не должно. База и размер
        // региона пула при этом сохраняются: их вернёт stack_free().
        info->task = nullptr;
        return nullptr;
    }

    t->save_sp(sp);
    return sp;
}

void embed::memory_manager::task_unregister(TaskHandler t) noexcept {
    task_slot* slot = find_slot(t);
    if (slot == nullptr) return;

    // Регион пула остаётся на учёте: вернуть его в пул обязан вызывающий
    // через stack_free(), и только с вершины (LIFO). Прежнее «*slot = {}»
    // стирало и базу, и размер — после remove_task() регион становился
    // невидимым, и его слова утекали.
    if (is_pool_region(slot->stack_base, slot->stack_words)) {
        slot->task       = nullptr;
        slot->high_water = slot->stack_words;
        return;
    }

    // Статический стек: учёт снимается целиком, место в таблице освобождается
    // (в пуле освобождать нечего — стек выделял вызывающий).
    *slot = task_slot{};
}

// ──────────── Stack pool ────────────
//
// Пул — СТЕК: регионы выдаются от вершины (bump-указатель stack_pool_used_) и
// возвращаются только с вершины.

uint32_t* embed::memory_manager::stack_alloc(Size words) noexcept {
    if (!initialized_ || words == 0u || stack_pool_size_ == 0u) return nullptr;

    // Запись учёта нужна ДО выдачи региона: иначе регион уходил бы наружу без
    // учёта (прежний код сначала увеличивал учёт, а слот подбирал потом и «как
    // повезёт» — отсюда и «сироты», и две записи на одну базу).
    task_slot* info = find_free_slot();
    if (info == nullptr) return nullptr;

    if (words > stack_pool_size_ - stack_pool_used_) return nullptr;

    uint32_t* base = stack_pool_base_ + stack_pool_used_;
    stack_pool_used_ += words;

    // task == nullptr, но база задана — «регион выделен, задаче ещё не отдан».
    // Отличить его от полностью свободной записи позволяет именно stack_base.
    info->task        = nullptr;
    info->stack_base  = base;
    info->stack_words = words;
    info->high_water  = words;
    return base;
}

embed::Status embed::memory_manager::stack_free(uint32_t* stack) noexcept {
    if (stack == nullptr) return Status::InvalidArg;
    if (!initialized_ || stack_pool_size_ == 0u) return Status::Unavailable;

    task_slot* slot = find_slot_by_stack(stack);
    if (slot == nullptr) return Status::NotFound;                 // чужой / уже возвращён
    if (!is_pool_region(slot->stack_base, slot->stack_words)) {
        return Status::NotFound;                                  // статический массив
    }
    if (slot->task != nullptr) return Status::Busy;                // стек ещё у задачи

    // ГЛАВНАЯ проверка: возвращать можно только ВЕРШИНУ пула. Без неё учёт
    // уменьшался бы на размер любого региона, вершина уезжала бы вниз, и
    // следующий stack_alloc() выдал бы участок живого стека.
    if (slot->stack_base + slot->stack_words != stack_pool_base_ + stack_pool_used_) {
        return Status::NotSupported;
    }

    stack_pool_used_ -= slot->stack_words;
    *slot = task_slot{};
    return Status::Ok;
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
