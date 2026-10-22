// sheduler.cpp
#include "sheduler.hpp"
#include "../memory/memory_manager.hpp"
#include "../../power/power.hpp"
#include "../../fault/fault.hpp"

namespace embed {

namespace {
inline const platform::Ops& P() noexcept { return platform::ops(); }
} // namespace

// ─────────────────────────── Жизненный цикл ─────────────────────────────────

Status scheduler::init() noexcept {
    if (initialized_) return Status::AlreadyExists;

    // Загружаем посмертную запись (если прошлый запуск упал). Печать — дело
    // приложения: до старта UART/ITM лог может быть ещё не готов.
#if !defined(MCFRAME_FAULT_REPORT) || (MCFRAME_FAULT_REPORT)
    (void)fault::init();
#endif
    // Даже в «тихом» режиме отдельные Fault-исключения должны попадать в
    // свои обработчики, а не только в HardFault.
    if (P().fault_enable) P().fault_enable();

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
            power::Supervisor& supervisor = power::Supervisor::instance();
            for (;;) {
                g_memory_manager.update_all_high_water();
                // Простой отдаётся супервизору энергосбережения, если он
                // инициализирован: он решает, остановить ли ядро (Sleep) или
                // погасить тактирование (DeepSleep). Без супервизора
                // поведение прежнее — платформенный WFI.
                if (supervisor.initialized()) supervisor.on_idle();
                else if (P().idle)            P().idle();
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

    const uint32_t self_core = P().current_core();

    // Снимаем со всех очередей. Если задача была ТЕКУЩЕЙ на этом ядре, одного
    // обнуления current мало: задачу уже нет ни в ready, ни в sleep_list,
    // поэтому вытеснить её некому — она продолжала бы исполняться до первого
    // блокирующего вызова, а следующее переключение контекста заставало бы
    // current == nullptr.
    bool was_current = false;
    for (uint32_t i = 0; i < core_count_; ++i) {
        ready_remove(cores_[i], t);
        if (cores_[i].current == t) {
            cores_[i].current = nullptr;
            if (i == self_core) was_current = true;
            // На ДРУГОМ ядре новую задачу выберет его собственный тик:
            // разбудить чужое ядро отсюда нечем — IPI нет в контракте
            // платформы (platform::Ops).
        }
    }
    sleep_list_.remove(t);
    if (t->wait_queue_ != nullptr) (void)t->wait_queue_->remove(t);
    t->clear_wake_pending();

    // Освобождаем ресурсы мониторинга: раньше записи утекали (их всего 32).
    // Пул стеков не трогаем: статический массив освобождать не нужно, а регион
    // stack_alloc() ОСТАЁТСЯ на учёте — его возвращает вызывающий через
    // stack_free(), и только с вершины пула (LIFO).
    g_memory_manager.task_unregister(t);
    t->set_done();

    all_tasks_.remove(t);

    if (was_current) {
        // Уступаем ядро. Контекст удалённой задачи НЕ сохраняем: current уже
        // обнулён, и context_switch() пропустит сохранение — задача не оживёт.
        unlock_early(lock);
        yield();
    }
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
    // kMaxCores == 1 — самый частый случай для МКУ: ядро одно, искать нечего.
    // Проверка обязана быть на этапе КОМПИЛЯЦИИ: иначе GCC видит цикл,
    // который может обратиться к cores_[1] при объявленном размере 1,
    // и справедливо выдаёт -Warray-bounds.
    if constexpr (kMaxCores == 1u) {
        return 0u;
    } else {
        uint32_t best = 0u;
        uint32_t min_load = cores_[0].ready_count + (cores_[0].current ? 1u : 0u);
        for (uint32_t i = 1u; i < core_count_ && i < kMaxCores; ++i) {
            const uint32_t load = cores_[i].ready_count + (cores_[i].current ? 1u : 0u);
            if (load < min_load) { min_load = load; best = i; }
        }
        return best;
    }
}

/// Единственная точка смены текущей задачи и запроса переключения.
void scheduler::switch_to(core_state& core, TaskHandler next) noexcept {
    if (next == nullptr || next == core.current) return;
    ready_remove(core, next);
    next->set_running();
    ++next->switches_;
    core.current = next;
}

/// Единственная точка ЗАПРОСА переключения: платформа выставляет PendSV
/// (на mock — заглушка), а счётчик запросов остаётся честным для диагностики
/// и тестов. Если платформа переключать контекст не умеет, запроса нет.
void scheduler::request_context_switch() noexcept {
    if (!P().trigger_context_switch) return;
    ++switch_requests_;
    P().trigger_context_switch();
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

    // ПЕРВОЕ переключение делает сама платформа, а не обработчик PendSV, и это
    // не оптимизация, а необходимость: уходящего контекста ещё нет — ядро
    // работает в потоковом режиме на MSP, а PSP не инициализирован вовсе.
    // Обработчик переключения начинается с сохранения R4-R11 на стек ТЕКУЩЕЙ
    // задачи (по PSP), то есть записал бы регистры по мусорному адресу.
    // platform::Ops::start_first_task() — единственное место, которое умеет
    // «войти» в задачу без сохранения предыдущей.
    if (P().start_first_task) {
        P().start_first_task(first->load_sp());
        return Status::Ok;
    }

    // Платформа умеет только просить переключение — отдаём запуск обработчику.
    request_context_switch();
    return Status::Ok;
}

void scheduler::yield() noexcept {
    request_context_switch();
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
    if (core >= core_count_) { unlock_early(lock); return Status::Fault; }

    TaskHandler cur = cores_[core].current;
    if (!cur) { unlock_early(lock); return Status::NotFound; }

    // Новый вызов — новый дедлайн: старый флаг не должен «просочиться».
    cur->timed_out_ = false;

    // Пробуждение пришло, пока задача ещё running (см. wake_pending_):
    // засыпать нельзя, иначе оно потеряется.
    if (cur->wake_pending()) {
        cur->clear_wake_pending();
        unlock_early(lock);
        return Status::Ok;
    }

    if (!cur->block_on(obj, ticks)) { unlock_early(lock); return Status::Fault; }

    if (ticks > 0u) {
        if (!sleep_list_.push_back(cur)) {
            cur->unblock();
            unlock_early(lock);
            return Status::AlreadyExists;
        }
    }
#if MCFRAME_TEST_HOOKS
    if (block_hook_ != nullptr) block_hook_(block_hook_ctx_);
#endif
    unlock_early(lock);
    yield();

    if (cur->timed_out()) {
        cur->timed_out_ = false;
        return Status::Timeout;
    }
    return Status::Ok;
}

Status scheduler::block_on_locked(Lock& lock, wait_queue& q, void* obj,
                                  uint32_t ticks) noexcept {
    const uint32_t core = P().current_core();
    if (core >= core_count_) { unlock_early(lock); return Status::Fault; }

    TaskHandler cur = cores_[core].current;
    if (!cur) { unlock_early(lock); return Status::NotFound; }

    // Новый вызов — новый дедлайн.
    cur->timed_out_ = false;

    // Проверка и постановка в очередь — под ТЕМ ЖЕ Lock, который держит
    // вызывающий. Именно это закрывает окно потерянного пробуждения.
    if (cur->wake_pending()) {
        cur->clear_wake_pending();
        unlock_early(lock);
        return Status::Ok;
    }

    if (!cur->block_on(obj, ticks)) { unlock_early(lock); return Status::Fault; }
    if (!q.push(cur)) {
        cur->unblock();
        unlock_early(lock);
        return Status::AlreadyExists;
    }

    if (ticks > 0u) {
        if (!sleep_list_.push_back(cur)) {
            (void)q.remove(cur);
            cur->unblock();
            unlock_early(lock);
            return Status::AlreadyExists;
        }
    }

#if MCFRAME_TEST_HOOKS
    // Задача уже в очереди объекта и (при таймауте) в sleep_list_.
    // Хук может сигналить/выставить событие — именно это проверяет тест
    // потерянного пробуждения.
    if (block_hook_ != nullptr) block_hook_(block_hook_ctx_);
#endif

    unlock_early(lock);
    yield();

    if (cur->timed_out()) {
        cur->timed_out_ = false;
        return Status::Timeout;
    }
    return Status::Ok;
}

bool scheduler::unblock_one(wait_queue& q) noexcept {
    Lock lock;

    TaskHandler t = q.pop_highest();
    if (!t) return false;

    // Снимаем с таймаута: задача проснулась по событию, а не по сроку.
    sleep_list_.remove(t);
    t->timed_out_ = false;
    t->clear_wake_pending();
    if (!t->unblock()) return false;

    const uint32_t core = t->is_pinned() ? t->core() : find_least_loaded_core();
    if (core >= core_count_) return false;
    if (!ready_push(cores_[core], t)) return false;
    check_preempt(cores_[core]);
    return true;
}

Size scheduler::unblock_all(wait_queue& q) noexcept {
    Lock lock;
    Size woken = 0u;

    for (;;) {
        TaskHandler t = q.pop_highest();
        if (t == nullptr) break;

        sleep_list_.remove(t);
        t->timed_out_ = false;
        t->clear_wake_pending();
        if (!t->unblock()) continue;

        const uint32_t core = t->is_pinned() ? t->core() : find_least_loaded_core();
        if (core >= core_count_) continue;
        if (!ready_push(cores_[core], t)) continue;
        check_preempt(cores_[core]);
        ++woken;
    }
    return woken;
}

bool scheduler::unblock_one(void* obj) noexcept {
    Lock lock;

    // Совместимость: блокировка без очереди (async) и старый API.
    // Для примитивов используется очередь ожидания — там порядок задан
    // приоритетом, а не порядком регистрации задач.
    for (auto it = all_tasks_.begin(); !it.done(); it.advance()) {
        TaskHandler t = it.get();
        if (!t->is_waiting_on(obj)) continue;

        if (t->wait_queue_ != nullptr) (void)t->wait_queue_->remove(t);
        sleep_list_.remove(t);
        t->timed_out_ = false;
        t->clear_wake_pending();
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

    // Задача ЕЩЁ выполняется: запоминаем пробуждение. Следующий block_on()
    // увидит флаг и вернётся, не засыпая. Без этого async-модуль терял
    // завершение, пришедшее между park() и block_on().
    if (t->state() == task_state::running) {
        t->set_wake_pending();
        return Status::Ok;
    }

    if (t->state() == task_state::suspended) {
        // Вытесненная задача уже стоит в очереди готовых: resume() — всё,
        // что нужно. Повторный ready_push() вернул бы AlreadyExists.
        t->resume();
        return Status::Ok;
    }

    if (t->state() != task_state::blocked &&
        t->state() != task_state::sleeping) {
        return Status::Busy;
    }

    if (t->wait_queue_ != nullptr) (void)t->wait_queue_->remove(t);
    sleep_list_.remove(t);
    t->timed_out_ = false;
    t->clear_wake_pending();

    if (!t->unblock()) return Status::Busy;

    const uint32_t core = (core_id < core_count_)
        ? core_id : (t->is_pinned() ? t->core() : find_least_loaded_core());
    if (core >= core_count_) return Status::Fault;

    if (!ready_push(cores_[core], t)) return Status::AlreadyExists;
    check_preempt(cores_[core]);
    return Status::Ok;
}

Status scheduler::set_priority_locked(TaskHandler t, uint32_t priority,
                                      bool update_base) noexcept {
    if (!t) return Status::InvalidArg;
    if (!all_tasks_.contains(t) && t != &idle_tcb_) return Status::NotFound;
    if (priority >= kMaxPriority) priority = kMaxPriority - 1u;

    if (update_base) t->base_priority_ = priority;
    if (t->priority_ == priority) return Status::Ok;

    // Если задача в очереди готовых — переставить её в другую очередь.
    // ready_remove() смотрит на СТАРЫЙ priority_, поэтому порядок важен.
    bool requeued = false;
    for (uint32_t i = 0; i < core_count_; ++i) {
        const uint32_t old = (t->priority_ < kMaxPriority)
            ? t->priority_ : (kMaxPriority - 1u);
        if (!cores_[i].ready[old].contains(t)) continue;

        ready_remove(cores_[i], t);
        t->priority_ = priority;
        if (!ready_push(cores_[i], t)) {
            // Вернуть как было, чтобы не потерять задачу из очереди.
            t->priority_ = old;
            (void)ready_push(cores_[i], t);
            return Status::Fault;
        }
        requeued = true;
        if (i == P().current_core()) check_preempt(cores_[i]);
        break;
    }

    if (!requeued) {
        t->priority_ = priority;
        // Выполняющаяся задача могла стать ниже — уступаем более приоритетной.
        const uint32_t core = P().current_core();
        if (core < core_count_ && cores_[core].current == t) {
            check_preempt(cores_[core]);
        }
    }
    return Status::Ok;
}

Status scheduler::set_priority(TaskHandler t, uint32_t priority) noexcept {
    Lock lock;
    return set_priority_locked(t, priority, true);
}

Status scheduler::set_effective_priority(TaskHandler t, uint32_t priority) noexcept {
    Lock lock;
    return set_priority_locked(t, priority, false);
}

Status scheduler::exit() noexcept {
    Lock lock;
    const uint32_t core = P().current_core();
    if (core >= core_count_) return Status::Fault;

    TaskHandler cur = cores_[core].current;
    if (!cur) return Status::NotFound;

    // idle — системная задача. Её «завершение» оставило бы ядро без задачи,
    // которую можно запустить, когда все остальные спят: pick_next() вернул бы
    // nullptr, и context_switch() спасала бы только аварийная подстановка idle.
    if (cur == &idle_tcb_) return Status::PermissionDenied;

    cur->set_done();
    all_tasks_.remove(cur);
    sleep_list_.remove(cur);
    ready_remove(cores_[core], cur);

    // КЛЮЧЕВОЕ: current обнуляем ДО переключения. Во-первых, ядру нужен новый
    // current. Во-вторых, context_switch() сохраняет контекст именно у
    // current — с обнулённым current он сохранение пропустит, и завершённая
    // задача не оживёт на следующем переключении (её стек может быть уже
    // отдан другому стеку/освобождён).
    cores_[core].current = nullptr;
    g_memory_manager.task_unregister(cur);

    // Отдаём ядро. Без этого завершившаяся задача продолжала бы исполняться
    // до первого блокирующего вызова.
    unlock_early(lock);
    yield();
    return Status::Ok;
}

// ─────────────────────────── Тик ────────────────────────────────────────────

void scheduler::tick() noexcept {
    if (!started_) return;

    Lock lock;
    ++tick_count_;

    const uint32_t core = P().current_core();

    // Учёт CPU по задачам: один указатель и одно 64-битное сложение за тик.
    // tick() уже в прерывании, поэтому Lock не нужен — current не меняется
    // между чтением и инкрементом.
    if (core < core_count_) {
        TaskHandler running = cores_[core].current;
        if (running != nullptr) ++running->runtime_ticks_;
    }

    // Сэмплируем глубину стеков по тику. Делается в прерывании, потому что
    // значения стека валидны именно в момент вытеснения; в простое этим
    // занимается idle-задача. Измерение не влияет на планирование.
    g_memory_manager.update_all_high_water();

    process_sleep_list(1u);

    if (core >= core_count_) return;

    // Round-robin: только внутри ОДНОГО приоритета. Если в очереди того же
    // приоритета есть готовая задача, текущая уходит в её хвост. Более
    // приоритетные задачи и так вытеснят её через check_preempt().
    if (time_slice_ticks_ != 0u && (tick_count_ % time_slice_ticks_) == 0u) {
        TaskHandler cur = cores_[core].current;
        if (cur != nullptr && cur != &idle_tcb_ && cur->is_running()) {
            const uint32_t prio = (cur->priority() < kMaxPriority)
                ? cur->priority() : (kMaxPriority - 1u);
            if (!cores_[core].ready[prio].empty()) {
                cur->suspend();
                if (ready_push(cores_[core], cur)) {
                    request_context_switch();
                } else {
                    cur->resume();   // не потерять задачу при сбое очереди
                }
            }
        }
    }

    // Вытеснение ТОЛЬКО ЗАПРАШИВАЕТСЯ, но не выполняется здесь.
    //
    // Прежде тик сам звал switch_to(next) — то есть переносил «текущую» задачу
    // на новую ДО сохранения контекста уходящей. Обработчик переключения,
    // получив такое состояние, сохранял бы контекст старой задачи в TCB новой
    // (в c.current уже стояла новая) и портил её SP. Поэтому единственная
    // точка смены задачи — context_switch()/prepare_context_switch(), а тик
    // лишь просит переключение: ровно то же делает check_preempt() после
    // пробуждения задач.
    check_preempt(cores_[core]);
}

void scheduler::advance_ticks(uint32_t ticks) noexcept {
    if (!started_ || ticks == 0u) return;

    Lock lock;
    tick_count_ += ticks;
    g_memory_manager.update_all_high_water();
    process_sleep_list(ticks);

    // Разбуженные задачи попадают в ready, но вытеснения здесь не делаем:
    // мы в контексте задачи, а не прерывания, и вернуть ядро обязана
    // вызывающая сторона (idle-задача сразу уступает через yield()).
}

uint32_t scheduler::next_sleep_ticks() const noexcept {
    uint32_t best = kNoDeadline;
    for (auto it = sleep_list_.safe_begin(); !it.done(); it.advance()) {
        const uint32_t d = it.get()->delay_ticks();
        if (d < best) best = d;
    }
    return best;
}

void scheduler::process_sleep_list(uint32_t ticks_step) noexcept {
    // Задачи, чья задержка истекла, собираем отдельно: нельзя мутировать
    // список во время обхода.
    task_queue_list woken;

    for (auto it = sleep_list_.safe_begin(); !it.done(); it.advance()) {
        TaskHandler t = it.get();
        if (!t->advance_delay(ticks_step)) continue;   // ещё спит

        sleep_list_.remove(t);

        // Задача с таймаутом стоит ещё и в очереди ожидания объекта:
        // снимаем её оттуда, иначе unblock_one() разбудил бы её позже
        // «по событию» уже после истечения срока.
        if (t->wait_queue_ != nullptr) (void)t->wait_queue_->remove(t);

        // timed_out_ — только для block_on(t != 0), а не для sleep_for:
        // иначе обычный сон оставлял бы флаг, и следующий block_on()
        // вернул бы Timeout, не проспав ни тика.
        if (t->deadline_armed_) t->timed_out_ = true;

        t->wake_from_sleep();
        t->wait_obj_ = nullptr;
        if (!woken.push_back(t)) t->unblock();   // не удалось — просто разбудим
    }

    while (TaskHandler t = woken.pop_front()) {
        const uint32_t core = t->is_pinned() ? t->core() : find_least_loaded_core();
        if (core >= core_count_) continue;
        ready_push(cores_[core], t);
        check_preempt(cores_[core]);
    }
}

Size scheduler::task_snapshot(TaskHandler* out, Size max) noexcept {
    if (out == nullptr || max == 0u) return 0u;

    Lock lock;
    Size n = 0u;
    for (auto it = all_tasks_.begin(); !it.done() && n < max; it.advance()) {
        out[n++] = it.get();
    }
    if (n < max) out[n++] = &idle_tcb_;
    return n;
}

void scheduler::check_preempt(core_state& core) noexcept {
    if (!core.current || !core.current->is_running()) return;
    TaskHandler next = pick_next(core);
    if (next && next->priority() < core.current->priority()) {
        request_context_switch();
    }
}

// ─────────────────────────── Смена контекста ────────────────────────────────

bool scheduler::stack_guard_ok(TaskHandler t) const noexcept {
    if (t == nullptr || t->stack_base_ == nullptr) return true;
    return t->stack_base_[0] == kStackGuardMagic;
}

uint32_t* scheduler::prepare_context_switch(uint32_t* saved_sp) noexcept {
    const uint32_t core_id = P().current_core();
    // Ядро не зарегистрировано: переключаться некуда — продолжаем прежний
    // контекст (его кадр только что сохранён, так что он цел).
    if (core_id >= core_count_) return saved_sp;

    core_state& c = cores_[core_id];

    if (c.current) {
        // Дно стека — до сохранения контекста: если оно затёрто, сохранять
        // уже некуда, а отчёт должен назвать именно эту задачу.
        if (!stack_guard_ok(c.current)) {
            fault::stack_overflow(c.current->name());
        }
        if (saved_sp) c.current->save_sp(saved_sp);

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

    // Загружать нечего (планировщик не инициализирован): оставляем ядро на
    // прежнем контексте, а не уходим по nullptr — обработчик загрузил бы
    // регистры из адреса 0.
    if (next == nullptr || next->load_sp() == nullptr) return saved_sp;

    ready_remove(c, next);
    next->set_running();
    ++next->switches_;
    c.current = next;
    return next->load_sp();
}

void scheduler::context_switch() noexcept {
    uint32_t* sp = P().save_context ? P().save_context() : nullptr;
    uint32_t* next_sp = prepare_context_switch(sp);
    if (next_sp != nullptr && P().load_context) P().load_context(next_sp);
}

// Сильная реализация слабого хука из os/fault: пока планировщик слинкован,
// отчёт об аварии знает имя задачи. В загрузчике (без планировщика)
// остаётся слабая заглушка, и образ не разрастается.
extern "C" const char* mcframe_fault_task_name() noexcept {
    TaskHandler t = scheduler::instance().current_task();
    return (t != nullptr) ? t->name() : nullptr;
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

uint64_t now_ticks() noexcept {
    return scheduler::instance().now_ticks();
}

Status block_on(void* obj, uint32_t ticks) noexcept {
    return scheduler::instance().block_on(obj, ticks == 0u ? 0u : ticks);
}

Status block_on_locked(Lock& lock, wait_queue& q, void* obj,
                       uint32_t ticks) noexcept {
    return scheduler::instance().block_on_locked(lock, q, obj, ticks);
}

bool unblock_one(wait_queue& q) noexcept {
    return scheduler::instance().unblock_one(q);
}

Size unblock_all(wait_queue& q) noexcept {
    return scheduler::instance().unblock_all(q);
}

bool unblock_one(void* obj) noexcept {
    return scheduler::instance().unblock_one(obj);
}

bool wake(TaskHandler t) noexcept {
    // Принимаем и sleeping, и blocked, и suspended — задача могла уйти
    // в любой из этих состояний к моменту завершения операции.
    return scheduler::instance().wakeup(t) == Status::Ok;
}

Status set_priority(TaskHandler t, uint32_t priority) noexcept {
    return scheduler::instance().set_priority(t, priority);
}

Status set_effective_priority(TaskHandler t, uint32_t priority) noexcept {
    return scheduler::instance().set_effective_priority(t, priority);
}

void yield() noexcept {
    scheduler::instance().yield();
}

} // namespace embed::sched_api
