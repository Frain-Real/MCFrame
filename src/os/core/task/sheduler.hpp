// sheduler.hpp  (имя файла сохранено для совместимости)
#pragma once

#include "task.hpp"
#include "../../iplatform.hpp"
#include "../../config/os_config.hpp"

namespace embed {

/// Планировщик.
///
/// Ключевые отличия от прежней версии:
///  * singleton со статическим хранилищем — тик можно регистрировать
///    статическим хуком без контекста, поэтому НЕТ разыменования nullptr
///    и объект не живёт на стеке main();
///  * add_task() сам инициализирует стек и переводит задачу в ready —
///    раньше task::start() не вызывался никем, и ready-очередь всегда
///    оставалась пустой;
///  * idle-задача создаётся всегда, поэтому pick_next() не возвращает
///    nullptr, когда все задачи спят;
///  * маска непустых приоритетов делает выбор задачи O(1);
///  * context_switch() — единственное место, меняющее state задачи.
class scheduler {
public:
    /// Ёмкости берутся из os_config.hpp: они определяют размер статических
    /// очередей готовых задач и потому настраиваются под конкретный МКУ.
    static constexpr uint32_t kMaxCores    = config::kMaxCores;
    static constexpr uint32_t kMaxPriority = config::kMaxPriority;
    static constexpr uint32_t kIdleStackWords = 128;

    static scheduler& instance() noexcept {
        static scheduler s;
        return s;
    }

    /// Статический хук для TimerModule::RegisterSystemHook().
    /// Контекст не нужен — это и была причина падения с nullptr.
    static void tick_hook(void*) noexcept { instance().tick(); }

    scheduler(const scheduler&)            = delete;
    scheduler& operator=(const scheduler&) = delete;

    // ──────────── Жизненный цикл ────────────
    /// Инициализация. Возвращает ошибку, если менеджер памяти не готов:
    /// без пула стеков idle-задачу создать нельзя.
    Status init() noexcept;

    bool started() const noexcept { return started_; }

    /// Создать idle-задачу (вызывается из init, отдельно нужен редко).
    Status create_idle_task() noexcept;

    bool add_task(TaskHandler t, uint32_t* stack, Size stack_words,
                  uint32_t core_id = kAnyCore) noexcept;

    /// Удалить задачу: снять со всех очередей и освободить ресурсы.
    /// Если удаляется ТЕКУЩАЯ задача, ядро тут же получает новую: иначе
    /// удалённая задача продолжала бы исполняться до первого блокирующего
    /// вызова (её уже нет ни в ready, ни в sleep_list — вытеснить некому).
    /// Возврат: Ok | NotFound | InvalidArg | PermissionDenied (idle).
    Status remove_task(TaskHandler t) noexcept;

    /// Запустить планировщик. Не возвращается.
    Status start() noexcept;

    void yield() noexcept;

    // ──────────── API задачи ────────────
    Status sleep() noexcept;
    Status sleep_for(uint32_t ticks) noexcept;
    Status wakeup(TaskHandler t, uint32_t core_id = kAnyCore) noexcept;

    /// Завершить ТЕКУЩУЮ задачу: снять со всех очередей и ОТДАТЬ ЯДРО.
    /// Возврат: Ok | NotFound (текущей задачи нет) | PermissionDenied (idle).
    ///
    /// ВНИМАНИЕ: после возврата задача выполняет ещё несколько инструкций —
    /// до входа в PendSV она остаётся на своём стеке. Поэтому освобождать её
    /// стек (stack_free) обязан ВНЕШНИЙ код и только после того, как задача
    /// действительно покинула ядро.
    Status exit() noexcept;

    /// Заблокировать текущую задачу на объекте (семафор/мьютекс).
    /// ticks == 0 — без таймаута.
    Status block_on(void* obj, uint32_t ticks = 0) noexcept;
    /// Разбудить задачу, ждущую на объекте obj. Возвращает true, если
    /// кто-то был разбужен.
    bool   unblock_one(void* obj) noexcept;

    // ──────────── Тик и переключение ────────────
    void tick() noexcept;
    void context_switch() noexcept;

    // ──────────── Диагностика ────────────
    TaskHandler current_task() noexcept {
        const uint32_t core = platform::ops().current_core();
        return (core < core_count_) ? cores_[core].current : nullptr;
    }
    Size     task_count()  const noexcept { return all_tasks_.size(); }
    uint32_t core_count()  const noexcept { return core_count_; }
    uint32_t ready_count(uint32_t core_id) const noexcept {
        if (core_id >= core_count_) return 0u;
        return cores_[core_id].ready_count;
    }
    Size sleep_count() const noexcept { return sleep_list_.size(); }
    uint64_t tick_count() const noexcept { return tick_count_; }

    /// Сколько раз ядро просили переключить контекст (диагностика).
    ///
    /// Нужно там, где платформенный trigger_context_switch() — заглушка
    /// (mock-сборка): по счётчику видно, что задача действительно УСТУПИЛА
    /// ядро, а не просто снялась со всех очередей и продолжила исполняться.
    Size switch_requests() const noexcept { return switch_requests_; }

    /// Статическое потребление RAM планировщиком (для отчётов о памяти).
    /// Растёт как kMaxCores * kMaxPriority * sizeof(очередь) — поэтому
    /// ёмкости вынесены в config/os_config.hpp.
    static constexpr Size static_bytes() noexcept { return sizeof(scheduler); }

private:
    scheduler() noexcept = default;

    struct core_state {
        TaskHandler          current = nullptr;
        task_queue_list    ready[kMaxPriority];
        uint32_t          ready_count = 0;
        uint32_t          ready_mask  = 0;   // бит p = ready[p] непуст
        uint32_t          core_id     = 0;
    };

    core_state           cores_[kMaxCores]{};
    uint32_t             core_count_ = 0;

    task_queue_list      sleep_list_;
    task_registry        all_tasks_;

    task                 idle_tcb_{};
    uint32_t             idle_stack_[kIdleStackWords]{};

    bool                 started_    = false;
    bool                 initialized_ = false;
    uint64_t             tick_count_ = 0;
    Size                 switch_requests_ = 0;   // диагностика (см. switch_requests)

    uint32_t find_least_loaded_core() const noexcept;
    TaskHandler pick_next(core_state& core) const noexcept;
    bool ready_push(core_state& core, TaskHandler t) noexcept;   // false, если уже в очереди
    void ready_remove(core_state& core, TaskHandler t) noexcept;
    void check_preempt(core_state& core) noexcept;
    void process_sleep_list() noexcept;
    void switch_to(core_state& core, TaskHandler next) noexcept;
    /// Единственная точка запроса переключения: считает запросы и дёргает
    /// платформу. Прямые вызовы P().trigger_context_switch() мимо неё
    /// потеряли бы диагностику.
    void request_context_switch() noexcept;
};

}
