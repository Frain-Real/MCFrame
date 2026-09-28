#include "sheduler.hpp"

// Инит 
void embed::scheduler::init() noexcept {
    core_count_ = platform_core_count();
    if (core_count_ > kMaxCores) core_count_ = kMaxCores;
    if (core_count_ == 0) core_count_ = 1;
    for (uint32_t i = 0; i < core_count_; ++i) {
        cores_[i].core_id     = i;
        cores_[i].current     = nullptr;
        cores_[i].ready_count = 0;
        for (uint32_t p = 0; p < kMaxPriority; ++p) cores_[i].ready[p].clear();
    }
    sleep_list_.clear();
    all_tasks_.clear();
    started_ = false;
}

// Работа с задачами
bool embed::scheduler::add_task(TaskHandler t, uint32_t core_id) noexcept {
    if (!t || all_tasks_.contains(t)) return false;
    platform_critical_enter();
    if (core_id >= core_count_) core_id = find_least_loaded_core();
    all_tasks_.push_back(t);
    if (t->is_ready()) ready_push(cores_[core_id], t);
    platform_critical_exit();
    return true;
}
void embed::scheduler::remove_task(TaskHandler t) noexcept {
    if (!t) return;
    platform_critical_enter();
    for (uint32_t i = 0; i < core_count_; ++i) ready_remove(cores_[i], t);
    if (sleep_list_.contains(t)) sleep_list_.remove(t);
    if (all_tasks_.contains(t)) all_tasks_.remove(t);
    platform_critical_exit();
}

// Lifecycle

void embed::scheduler::start() noexcept {
    started_ = true;
    for (uint32_t i = 0; i < core_count_; ++i) {
        TaskHandler next = pick_next(cores_[i]);
        if (next) {
            ready_remove(cores_[i], next);
            next->set_running();
            cores_[i].current = next;
        }
    }
    uint32_t* first_sp = nullptr;
    if (cores_[0].current) {
        first_sp = cores_[0].current->load_sp();
    }
    platform_start_first_task(first_sp);
}
void embed::scheduler::yield() noexcept { platform_trigger_context_switch(); }

// Task API
void embed::scheduler::sleep() noexcept {
    platform_critical_enter();
    uint32_t core = platform_current_core();
    TaskHandler cur = cores_[core].current;
    if (cur) { cur->suspend(); cores_[core].current = nullptr; }
    platform_critical_exit();
    yield();
}
void embed::scheduler::sleep_for(uint32_t ticks) noexcept {
    if (ticks == 0) return;
    platform_critical_enter();
    uint32_t core = platform_current_core();
    TaskHandler cur = cores_[core].current;
    if (cur) {
        cur->sleep_for(ticks);
        sleep_list_.push_back(cur);
        cores_[core].current = nullptr;
    }
    platform_critical_exit();
    yield();
}
void embed::scheduler::wakeup(TaskHandler t, uint32_t core_id) noexcept {
    if (!t) return;
    platform_critical_enter();
    if (t->state() == task_state::suspended) {
        t->resume();
        if (core_id >= core_count_) core_id = find_least_loaded_core();
        ready_push(cores_[core_id], t);
        check_preempt(cores_[core_id]);
    }
    platform_critical_exit();
}
void embed::scheduler::exit() noexcept {
    platform_critical_enter();
    uint32_t core = platform_current_core();
    TaskHandler cur = cores_[core].current;
    if (cur) { cur->set_done(); cores_[core].current = nullptr; }
    platform_critical_exit();
    yield();
}

// Тик (SysTick_Handler)
void embed::scheduler::tick() noexcept {
    if (!started_) return;
    platform_critical_enter();
    process_sleep_list();
    uint32_t core = platform_current_core();
    TaskHandler cur = cores_[core].current;
    if (cur && cur->state() == task_state::running) {
        TaskHandler next = pick_next(cores_[core]);
        if (next && next->priority() < cur->priority()) {
            cur->suspend();
            ready_push(cores_[core], cur);
            ready_remove(cores_[core], next);
            next->set_running();
            cores_[core].current = next;
        }
    }
    platform_critical_exit();
}

// Смена контекста (PendSV_Handler)
void embed::scheduler::context_switch() noexcept {
    uint32_t core = platform_current_core();
    core_state& c = cores_[core];
    if (c.current) {
        uint32_t* sp = platform_save_context();
        c.current->save_sp(sp);
        if (c.current->state() == task_state::running) {
            c.current->suspend();
            ready_push(c, c.current);
        }
    }
    TaskHandler next = pick_next(c);
    if (next) {
        ready_remove(c, next);
        next->set_running();
        c.current = next;
        platform_load_context(next->load_sp());
    }
}

uint32_t embed::scheduler::find_least_loaded_core() noexcept {
    uint32_t best_core = 0;
    uint32_t min_count = cores_[0].ready_count;
    for (uint32_t i = 1; i < core_count_; ++i) {
        if (cores_[i].ready_count < min_count) {
            min_count = cores_[i].ready_count;
            best_core = i;
        }
    }
    return best_core;
}

embed::TaskHandler embed::scheduler::pick_next(core_state& core) noexcept {
    for (uint32_t p = 0; p < kMaxPriority; ++p) {
        if (!core.ready[p].empty()) return core.ready[p].front();
    }
    return nullptr;
}

void embed::scheduler::ready_push(core_state& core, TaskHandler t) noexcept {
    if (!t) return;
    uint32_t prio = t->priority();
    if (prio >= kMaxPriority) prio = kMaxPriority - 1;
    core.ready[prio].push_back(t);
    ++core.ready_count;
}

void embed::scheduler::ready_remove(core_state& core, TaskHandler t) noexcept {
    if (!t) return;
    uint32_t prio = t->priority();
    if (prio >= kMaxPriority) prio = kMaxPriority - 1;
    if (core.ready[prio].contains(t)) {
        core.ready[prio].remove(t);
        --core.ready_count;
    }
}

void embed::scheduler::check_preempt(core_state& core) noexcept {
    if (!core.current) return;
    if (core.current->state() != task_state::running) return;
    TaskHandler next = pick_next(core);
    if (next && next->priority() < core.current->priority()) {
        platform_trigger_context_switch();
    }
}

void embed::scheduler::process_sleep_list() noexcept {
    intrusive_list<task> to_wake;
    for (auto it = sleep_list_.begin(); it != sleep_list_.end(); ) {
        TaskHandler t = &(*it);
        ++it;
        if (t->tick_delay()) {
            sleep_list_.remove(t);
            t->wake_from_sleep();
            to_wake.push_back(t);
        }
    }
    while (!to_wake.empty()) {
        TaskHandler t = to_wake.pop_front();
        uint32_t core_id = find_least_loaded_core();
        ready_push(cores_[core_id], t);
        check_preempt(cores_[core_id]);
    }
}