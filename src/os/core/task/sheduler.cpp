// sheduler.cpp
#include "sheduler.hpp"
#include "../memory/memory_manager.hpp"

namespace embed {

namespace {
inline const platform::Ops& P() noexcept { return platform::ops(); }
} // namespace

// ─────────────────────────── Жизненный цикл ─────────────────────────────────

Status scheduler::init() noexcept {
    if (initialized_) return Status::AlreadyExists;

    uint32_t cores = P().core_count ? P().core_count() : 1u;
    if (cores == 0u) cores = 1u;
    if (cores > kMaxCores) cores = kMaxCores;
    core_count_ = cores;

    for (uint32_t i = 0; i < core_count_; ++i) {
        cores_[i].core_id     = i;
        cores_[i].current     = nullptr;
        cores_[i].ready_count = 0u;
        cores_[i].ready_mask  = 0u;
        for (uint32_t p = 0; p < kMaxPriority; ++p) cores_[i].ready[p].clear();
    }
    sleep_list_.clear();
    all_tasks_.clear();
    started_     = false;
    tick_count_  = 0u;
    initialized_ = true;

    return create_idle_task();
}

Status scheduler::create_idle_task() noexcept {
    // idle обязателен: без него pick_next() вернёт nullptr, когда все
    // задачи спят, и context_switch() уйдёт в platform_load_context(nullptr).
    idle_tcb_.reset();
    const Status st = idle_tcb_.init(
        [](void*) noexcept {
            scheduler& s = scheduler::instance();
            for (;;) {
                g_memory_manager.update_all_high_water();
                if (P().idle) P().idle();      // __WFI, если нет готовой работы
                // На хосте (mock) idle возвращается сразу — уступаем,
                // чтобы не выедать CPU в тестах.
                s.yield();
            }
        },
        nullptr, "idle", kIdleStackWords, kMaxPriority - 1u);

    if (st != Status::Ok) return st;

    uint32_t* sp = g_memory_manager.task_stack_init(&idle_tcb_, idle_stack_, kIdleStackWords);
    if (sp == nullptr) return Status::NoMemory;

    if (!idle_tcb_.start()) return Status::Fault;

    if (!ready_push(cores_[0], &idle_tcb_)) return Status::AlreadyExists;
    return Status::Ok;
}

// ─────────────────────────── Работа с задачами ──────────────────────────────

bool scheduler::add_task(TaskHandler t, uint32_t* stack, Size stack_words,
                         uint32_t core_id) noexcept {
    if (!t || !stack || stack_words == 0u) return false;

    Lock lock;
    if (!initialized_)            return false;
    if (t == &idle_tcb_)          return false;   // idle создаётся отдельно
    if (all_tasks_.contains(t))   return false;

    // Инициализация стека: без неё sp_ == nullptr и первая же загрузка
    // контекста уронит систему.
    if (g_memory_manager.task_stack_init(t, stack, stack_words) == nullptr) return false;

    // Перевод idle -> ready. Именно этого вызова не было раньше, поэтому
    // задачи никогда не попадали в ready-очередь.
    // Идемпотентно: задача могла быть переведена в ready пользователем
    // (t.start()) до регистрации — это допустимо.
    if (!t->is_ready() && !t->start()) return false;

    if (!all_tasks_.add(t)) return false;

    const uint32_t core = (core_id < core_count_) ? core_id : find_least_loaded_core();
    if (!ready_push(cores_[core], t)) {
        all_tasks_.remove(t);
        return false;
    }
    return true;
}

Status scheduler::remove_task(TaskHandler t) noexcept {
    if (!t) return Status::InvalidArg;
    if (t == &idle_tcb_) return Status::PermissionDenied;

    Lock lock;
    if (!all_tasks_.contains(t)) return Status::NotFound;

    for (uint32_t i = 0; i < core_count_; ++i) {
        ready_remove(cores_[i], t);
        if (cores_[i].current == t) cores_[i].current = nullptr;
    }
    sleep_list_.remove(t);

    // Освобождаем ресурсы мониторинга: раньше записи утекали (их всего 32).
    // Пул стеков не трогаем: стек выделял вызывающий (статический массив),
    // а не stack_alloc().
    g_memory_manager.task_unregister(t);
    t->set_done();

    all_tasks_.remove(t);
    return Status::Ok;
}

// ─────────────────────────── Очереди готовых ────────────────────────────────

bool scheduler::ready_push(core_state& core, TaskHandler t) noexcept {
    if (!t) return false;
    uint32_t prio = t->priority();
    if (prio >= kMaxPriority) prio = kMaxPriority - 1u;

    if (!core.ready[prio].push_back(t)) return false;   // уже в очереди
    ++core.ready_count;
    core.ready_mask |= (1u << prio);
    return true;
}

void scheduler::ready_remove(core_state& core, TaskHandler t) noexcept {
    if (!t) return;
    uint32_t prio = t->priority();
    if (prio >= kMaxPriority) prio = kMaxPriority - 1u;

    if (core.ready[prio].remove(t)) {          // remove() проверяет владельца
        --core.ready_count;
        if (core.ready[prio].empty()) core.ready_mask &= ~(1u << prio);
    }
}

TaskHandler scheduler::pick_next(core_state& core) const noexcept {
    if (core.ready_mask == 0u) return nullptr;
    // O(1) вместо линейного скана 16 приоритетов.
    const uint32_t prio = static_cast<uint32_t>(__builtin_ctz(core.ready_mask));
    return core.ready[prio].front();
}

uint32_t scheduler::find_least_loaded_core() const noexcept {
    uint32_t best = 0u;
    uint32_t min_load = cores_[0].ready_count + (cores_[0].current ? 1u : 0u);
    for (uint32_t i = 1u; i < core_count_; ++i) {
        const uint32_t load = cores_[i].ready_count + (cores_[i].current ? 1u : 0u);
        if (load < min_load) { min_load = load; best = i; }
    }
    return best;
}

/// Единственная точка смены текущей задачи и запроса переключения.
void scheduler::switch_to(core_state& core, TaskHandler next) noexcept {
    if (next == nullptr || next == core.current) return;
    ready_remove(core, next);
    next->set_running();
    core.current = next;
}

// ─────────────────────────── Lifecycle ──────────────────────────────────────

Status scheduler::start() noexcept {
    if (!initialized_) return Status::Unavailable;
    if (started_)      return Status::AlreadyExists;

    Lock lock;
    started_ = true;

    // Первая задача на каждом ядре.
    for (uint32_t i = 0; i < core_count_; ++i) {
        switch_to(cores_[i], pick_next(cores_[i]));
    }

    TaskHandler first = cores_[0].current;
    if (!first || first->load_sp() == nullptr) return Status::Fault;

    if (P().trigger_context_switch) {
        // Полноценный запуск: уступаем планировщику через PendSV.
        P().trigger_context_switch();
        return Status::Ok;
    }
    P().start_first_task(first->load_sp());
    return Status::Ok;
}

void scheduler::yield() noexcept {
    if (P().trigger_context_switch) P().trigger_context_switch();
}

// ─────────────────────────── API задачи ─────────────────────────────────────

Status scheduler::sleep() noexcept {
    Lock lock;
    const uint32_t core = P().current_core();
    if (core >= core_count_) return Status::Fault;

    TaskHandler cur = cores_[core].current;
    if (!cur) return Status::NotFound;

    // Задача блокируется БЕЗ таймаута: она не попадает ни в ready, ни в
    // sleep_list, её разбудит только wakeup()/unblock_one().
    cur->block_on(nullptr, 0u);
    unlock_early(lock);
    yield();
    return Status::Ok;
}

Status scheduler::sleep_for(uint32_t ticks) noexcept {
    Lock lock;
    const uint32_t core = P().current_core();
    if (core >= core_count_) return Status::Fault;

    TaskHandler cur = cores_[core].current;
    if (!cur) return Status::NotFound;

    if (ticks == 0u) { unlock_early(lock); yield(); return Status::Ok; }
    if (!cur->sleep_for(ticks)) return Status::Fault;

    if (!sleep_list_.push_back(cur)) {
        cur->set_running();               // откат
        return Status::AlreadyExists;
    }
    unlock_early(lock);
    yield();
    return Status::Ok;
}

Status scheduler::block_on(void* obj, uint32_t ticks) noexcept {
    Lock lock;
    const uint32_t core = P().current_core();
    if (core >= core_count_) return Status::Fault;

    TaskHandler cur = cores_[core].current;
    if (!cur) return Status::NotFound;

    if (!cur->block_on(obj, ticks)) return Status::Fault;

    if (ticks > 0u) {
        if (!sleep_list_.push_back(cur)) {
            cur->unblock();
            return Status::AlreadyExists;
        }
    }
    unlock_early(lock);
    yield();
    return Status::Ok;
}

bool scheduler::unblock_one(void* obj) noexcept {
    Lock lock;

    // Ищем ждущего на этом объекте.
    for (auto it = all_tasks_.begin(); !it.done(); it.advance()) {
        TaskHandler t = it.get();
        if (t->wait_obj() != obj) continue;
        if (!t->is_blocked() && !t->is_sleeping()) continue;

        sleep_list_.remove(t);
        t->unblock();

        const uint32_t core = t->is_pinned() ? t->core() : find_least_loaded_core();
        if (core >= core_count_) continue;
        ready_push(cores_[core], t);
        check_preempt(cores_[core]);
        return true;
    }
    return false;
}

Status scheduler::wakeup(TaskHandler t, uint32_t core_id) noexcept {
    if (!t) return Status::InvalidArg;

    Lock lock;
    if (!all_tasks_.contains(t) && t != &idle_tcb_) return Status::NotFound;

    if (t->state() != task_state::suspended &&
        t->state() != task_state::blocked &&
        t->state() != task_state::sleeping) {
        return Status::Busy;
    }

    sleep_list_.remove(t);

    // unblock() покрывает blocked/sleeping, suspend()+resume() — вытесненную.
    if (!t->unblock()) {
        if (t->state() != task_state::suspended) return Status::Busy;
        t->resume();
    }

    const uint32_t core = (core_id < core_count_)
        ? core_id : (t->is_pinned() ? t->core() : find_least_loaded_core());
    if (core >= core_count_) return Status::Fault;

    if (!ready_push(cores_[core], t)) return Status::AlreadyExists;
    check_preempt(cores_[core]);
    return Status::Ok;
}

Status scheduler::exit() noexcept {
    Lock lock;
    const uint32_t core = P().current_core();
    if (core >= core_count_) return Status::Fault;

    TaskHandler cur = cores_[core].current;
    if (!cur) return Status::NotFound;

    cur->set_done();
    all_tasks_.remove(cur);
    sleep_list_.remove(cur);
    g_memory_manager.task_unregister(cur);
    return Status::Ok;
}

// ─────────────────────────── Тик ────────────────────────────────────────────

void scheduler::tick() noexcept {
    if (!started_) return;

    Lock lock;
    ++tick_count_;

    // Сэмплируем глубину стеков по тику. Делается в прерывании, потому что
    // значения стека валидны именно в момент вытеснения; в простое этим
    // занимается idle-задача. Измерение не влияет на планирование.
    g_memory_manager.update_all_high_water();

    process_sleep_list();

    const uint32_t core = P().current_core();
    if (core >= core_count_) return;

    TaskHandler cur = cores_[core].current;
    if (cur && cur->is_running()) {
        TaskHandler next = pick_next(cores_[core]);
        if (next && next->priority() < cur->priority()) {
            cur->suspend();
            ready_push(cores_[core], cur);
            switch_to(cores_[core], next);
            if (P().trigger_context_switch) P().trigger_context_switch();
        }
    }
}

void scheduler::process_sleep_list() noexcept {
    // Задачи, чья задержка истекла, собираем отдельно: нельзя мутировать
    // список во время обхода.
    task_queue_list woken;

    for (auto it = sleep_list_.safe_begin(); !it.done(); it.advance()) {
        TaskHandler t = it.get();
        if (!t->tick_delay()) continue;      // ещё спит

        sleep_list_.remove(t);
        t->wake_from_sleep();
        if (!woken.push_back(t)) t->unblock();   // не удалось — просто разбудим
    }

    while (TaskHandler t = woken.pop_front()) {
        const uint32_t core = t->is_pinned() ? t->core() : find_least_loaded_core();
        if (core >= core_count_) continue;
        ready_push(cores_[core], t);
        check_preempt(cores_[core]);
    }
}

void scheduler::check_preempt(core_state& core) noexcept {
    if (!core.current || !core.current->is_running()) return;
    TaskHandler next = pick_next(core);
    if (next && next->priority() < core.current->priority()) {
        if (P().trigger_context_switch) P().trigger_context_switch();
    }
}

// ─────────────────────────── Смена контекста ────────────────────────────────

void scheduler::context_switch() noexcept {
    const uint32_t core_id = P().current_core();
    if (core_id >= core_count_) return;

    core_state& c = cores_[core_id];

    if (c.current) {
        uint32_t* sp = P().save_context ? P().save_context() : nullptr;
        if (sp) c.current->save_sp(sp);

        // Возвращаем в ready ТОЛЬКО если задача не заблокировала себя сама
        // (sleep/sleep_for/block_on уже перевели её в sleeping/blocked).
        // Прежняя версия обнуляла current в sleep() до сохранения SP,
        // из-за чего задача терялась навсегда.
        if (c.current->is_running()) {
            c.current->suspend();
            ready_push(c, c.current);
        }
        c.current = nullptr;
    }

    TaskHandler next = pick_next(c);
    if (!next) next = &idle_tcb_;      // страховка: idle есть всегда

    if (next == nullptr) return;       // нечего запускать (init не вызывался)

    ready_remove(c, next);
    next->set_running();
    c.current = next;

    if (P().load_context) P().load_context(next->load_sp());
}

} // namespace embed

// ─────────────── Мост для примитивов (mutex/semaphore) ──────────────────────
// Примитивы не должны тянуть весь sheduler.hpp, поэтому получают только эти
// три операции. Они же гарантируют, что ожидание уходит в планировщик,
// а не в busy-wait.
namespace embed::sched_api {

TaskHandler current() noexcept {
    return scheduler::instance().current_task();
}

Status block_on(void* obj, uint32_t ticks) noexcept {
    return scheduler::instance().block_on(obj, ticks == 0u ? 0u : ticks);
}

bool unblock_one(void* obj) noexcept {
    return scheduler::instance().unblock_one(obj);
}

} // namespace embed::sched_api
