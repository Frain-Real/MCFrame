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

    /// Сколько тиков прошло с запуска планировщика (монотонно).
    /// База для АБСОЛЮТНЫХ таймаутов: примитивы считают deadline один раз,
    /// а не перезапускают ожидание после каждого ложного пробуждения.
    uint64_t now_ticks() const noexcept { return tick_count_; }

    /// Заблокировать текущую задачу БЕЗ очереди ожидания (совместимость).
    /// ticks == 0 — без таймаута. Используется async-модулем, который будит
    /// задачу адресно через wakeup(); примитивы обязаны использовать
    /// block_on_locked() — иначе остаётся окно потерянного пробуждения.
    Status block_on(void* obj, uint32_t ticks = 0) noexcept;

    /// Заблокировать текущую задачу, УЖЕ стоя под Lock и проверив предикат.
    ///
    /// Почему так: проверка «ресурс свободен?» и постановка в очередь
    /// ожидания обязаны быть одной критической секцией. Иначе прерывание
    /// между ними сигналит, unblock_one() не находит ещё не заблокированную
    /// задачу, и она засыпает навсегда (ticks == 0).
    ///
    /// Владение lock переходит сюда: функция сама делает unlock_early()
    /// перед yield(), поэтому деструктор Lock у вызывающего — no-op.
    /// Возврат: Ok (разбудили), Timeout (истёк ticks), Fault/NotFound/
    /// AlreadyExists. При AlreadyExists/Fault задача остаётся running.
    Status block_on_locked(Lock& lock, wait_queue& q, void* obj,
                           uint32_t ticks = 0) noexcept;

    /// Разбудить самую приоритетную задачу из очереди q.
    /// Возвращает true, если кто-то был разбужен.
    bool unblock_one(wait_queue& q) noexcept;

    /// Разбудить ВСЕХ из очереди q (EventFlags: каждый перепроверит маску).
    /// Возвращает число разбуженных.
    Size unblock_all(wait_queue& q) noexcept;

#if MCFRAME_TEST_HOOKS
    using BlockHook = void (*)(void*);
    /// Тестовый хук: вызывается в block_on_locked() ПОСЛЕ постановки задачи
    /// в очередь и до unlock. Именно здесь проверяется, что сигнал из ISR
    /// видит уже заблокированную задачу.
    void set_block_hook(BlockHook hook, void* ctx) noexcept {
        block_hook_ = hook;
        block_hook_ctx_ = ctx;
    }
#endif

    /// Совместимость: разбудить первую задачу, ждущую на объекте obj
    /// (линейный поиск). Новый код должен использовать unblock_one(q).
    bool unblock_one(void* obj) noexcept;

    /// Изменить приоритет задачи (назначенный и действующий).
    /// Если задача в очереди готовых — она переставляется; если выполняется —
    /// новый приоритет учтётся при следующем вытеснении.
    Status set_priority(TaskHandler t, uint32_t priority) noexcept;

    /// Изменить ТОЛЬКО действующий приоритет (для PI/ceiling): база,
    /// назначенная приложением, не меняется.
    Status set_effective_priority(TaskHandler t, uint32_t priority) noexcept;

    // ──────────── Тик и переключение ────────────
    void tick() noexcept;

    /// Квант round-robin для задач одного приоритета, тиков.
    /// 0 — выключить (строго приоритетное планирование). Включается на ходу
    /// и не влияет на задачи разных приоритетов.
    void     set_time_slice(uint32_t ticks) noexcept { time_slice_ticks_ = ticks; }
    uint32_t time_slice() const noexcept { return time_slice_ticks_; }

    /// Сменить контекст: сохранить SP уходящей задачи, выбрать следующую и
    /// загрузить её контекст. На МКУ эту работу делает обработчик PendSV,
    /// на хосте её вызывают тесты явно (переключения контекста там нет).
    void context_switch() noexcept;

    /// СЕРЕДИНА переключения: сохранить SP уходящей задачи (saved_sp — то,
    /// что вернул platform save_context(), либо nullptr, если сохранять нечем)
    /// и выбрать следующую задачу; возвращает SP следующей (или saved_sp,
    /// если переключаться некуда — тогда ядро продолжает прежний контекст).
    ///
    /// ЗАЧЕМ ОТДЕЛЬНО ОТ context_switch()
    /// Обработчик PendSV на МКУ обязан быть СБАЛАНСИРОВАННЫМ по MSP: он сам
    /// делает `bx lr` (EXC_RETURN), поэтому загрузка контекста не может
    /// происходить «изнутри C» — кадры обработчика остались бы в MSP и
    /// накапливались бы с каждым переключением (1 КБ MSP кончились бы за
    /// десятки переключений). Отсюда деление: asm сохраняет контекст, вызывает
    /// ЭТУ функцию (обычный вызов, который ВОЗВРАЩАЕТ SP следующей задачи),
    /// затем сам загружает контекст и выходит из исключения.
    ///
    /// Вызывать из обработчика: состояние задач меняется целиком здесь, и
    /// прерывания на это время должны быть запрещены (на МКУ это делает asm
    /// обработчика, см. platforms/stm32/platform_ops.cpp).
    uint32_t* prepare_context_switch(uint32_t* saved_sp) noexcept;

    /// Продвинуть задержки спящих задач сразу на ticks тиков.
    ///
    /// Нужно ровно в одном случае: монотонное время ОС продвинулось скачком
    /// (супервизор энергосбережения вернулся из глубокого сна, где SysTick
    /// не считал). Вызывать вместо ticks вызовов tick(): tick_count_ растёт
    /// сразу на ticks, а спящие задачи получают ту же семантику «задержка
    /// истекла не позже срока», что и при обычных тиках.
    ///
    /// Из прерывания вызывать не нужно: это контекст задачи (idle).
    void advance_ticks(uint32_t ticks) noexcept;

    // ──────────── Диагностика ────────────
    TaskHandler current_task() noexcept {
        const uint32_t core = platform::ops().current_core();
        return (core < core_count_) ? cores_[core].current : nullptr;
    }

    /// Исполняется ли прямо сейчас idle-задача этого ядра.
    ///
    /// Единственный дешёвый способ измерить загрузку CPU: в тике достаточно
    /// сравнить указатель. Именно так считает нагрузку супервизор
    /// энергосбережения (os/power/power.hpp); точность — один тик.
    bool idle_running() const noexcept {
        const uint32_t core = platform::ops().current_core();
        if (core >= core_count_) return false;
        return cores_[core].current == &idle_tcb_;
    }

    /// Цело дно стека задачи (сторожевой маркер kStackGuardMagic)?
    ///
    /// Проверка дешёвая (одно чтение), поэтому вызывается в переключении
    /// контекста для УХОДЯЩЕЙ задачи. Без MPU она не предотвращает порчу
    /// соседней памяти, но превращает молчаливое повреждение в аварийный
    /// отчёт с именем задачи и адресом.
    bool stack_guard_ok(TaskHandler t) const noexcept;

    /// Сколько тиков осталось ближайшей спящей задаче (kNoDeadline — таких нет).
    /// Основа для «горизонта сна»: просыпаться раньше срока задачи нельзя.
    uint32_t next_sleep_ticks() const noexcept;
    Size     task_count()  const noexcept { return all_tasks_.size(); }

    /// Снимок указателей на задачи (включая idle) без блокировки на время
    /// обхода: shell/ps не должен держать Lock, пока печатает.
    /// Возвращает число записей; порядок — реестр + idle последним.
    Size     task_snapshot(TaskHandler* out, Size max) noexcept;

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
    uint32_t             time_slice_ticks_ = config::kTimeSliceTicks;
#if MCFRAME_TEST_HOOKS
    BlockHook            block_hook_ = nullptr;
    void*                block_hook_ctx_ = nullptr;
#endif
    Size                 switch_requests_ = 0;   // диагностика (см. switch_requests)

    uint32_t find_least_loaded_core() const noexcept;
    TaskHandler pick_next(core_state& core) const noexcept;
    bool ready_push(core_state& core, TaskHandler t) noexcept;   // false, если уже в очереди
    void ready_remove(core_state& core, TaskHandler t) noexcept;
    Status set_priority_locked(TaskHandler t, uint32_t priority,
                               bool update_base) noexcept;
    void check_preempt(core_state& core) noexcept;
    /// ticks_step == 1 — обычный тик; больше — «догон» после скачка времени.
    void process_sleep_list(uint32_t ticks_step) noexcept;
    void switch_to(core_state& core, TaskHandler next) noexcept;
    /// Единственная точка запроса переключения: считает запросы и дёргает
    /// платформу. Прямые вызовы P().trigger_context_switch() мимо неё
    /// потеряли бы диагностику.
    void request_context_switch() noexcept;
};

}
