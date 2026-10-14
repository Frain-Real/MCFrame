// task.hpp
#pragma once
#include "../primitives/intrusive_list.hpp"
#include "../../common.hpp"
#include "../../config/os_config.hpp"

namespace embed {
    enum class task_type : uint8_t {
        static_task,   // TCB и стек выделены статически (глобально)
        dynamic_task   // TCB и стек выделены динамически (куча)
    };

    /// Состояние задачи с вытеснением.
    enum class task_state : uint8_t {
        idle,       // не инициализирована / не готова к запуску
        ready,      // готова к выполнению
        running,    // выполняется сейчас
        suspended,  // вытеснена: готова и стоит в ready-очереди
        sleeping,   // спит N тиков
        blocked,    // ждёт события (семафор/мьютекс) без таймаута в тиках
        done        // завершена
    };

    class task;
    using TaskHandler = task*;

    /// Любое ядро — задача может выполняться на любом ядре.
    static constexpr uint32_t kAnyCore = 0xFFFFFFFFu;

    /// Сколько задач планировщик может держать одновременно.
    /// Должно быть не меньше MCFRAME_MAX_TASKS (учёт менеджера памяти).
    static constexpr uint32_t kMaxTasks = 32;

    /// Задача.
    ///
    /// Наследует list_node<task> ПЕРВЫМ базовым классом: этот узел
    /// используется очередями планировщика (ready / sleep), поэтому
    /// преобразование узел <-> задача бесплатно.
    ///
    /// Реестр задач (все существующие задачи) реализован отдельным массивом
    /// object_registry, а не вторым узлом: один узел не может принадлежать
    /// двум спискам, и прежняя попытка совместить реестр с очередью готовых
    /// приводила к тому, что add_task() всегда возвращал ошибку.
    class task : public list_node<task> {
    public:
        uint32_t*   sp_          = nullptr;   // сохранённый SP
        task_func   fn_          = nullptr;   // функция задачи
        const char* name_        = nullptr;   // имя (для отладки)
        uint32_t    priority_    = 0;         // приоритет (0 = высший)
        uint32_t    stack_words_ = 0;         // выделено памяти (слов)

        task_type   type_        = task_type::static_task;
        task_state  state_       = task_state::idle;
        void*       ctx_         = nullptr;   // аргумент функции

        uint32_t    delay_ticks_ = 0;         // осталось тиков до пробуждения
        uint32_t    core_        = kAnyCore;  // привязка к ядру (affinity)
        void*       wait_obj_    = nullptr;   // объект, на котором задача ждёт

        task() noexcept = default;
        task(const task&)            = delete;
        task& operator=(const task&) = delete;

        /// Настройка задачи. Повторный вызов для активной задачи запрещён:
        /// иначе легко получить две записи в очереди готовых.
        Status init(task_func entry, void* ctx, const char* name,
                    uint32_t stack_words, uint32_t priority = 0,
                    task_type type = task_type::static_task,
                    uint32_t core = kAnyCore) noexcept {
            if (entry == nullptr || stack_words == 0u) return Status::InvalidArg;
            if (state_ != task_state::idle && state_ != task_state::done) {
                return Status::Busy;
            }
            if (priority >= config::kMaxPriority) priority = config::kMaxPriority - 1u;

            fn_          = entry;
            ctx_         = ctx;
            name_        = name;
            stack_words_ = stack_words;
            priority_    = priority;
            type_        = type;
            core_        = core;
            state_       = task_state::idle;
            delay_ticks_ = 0;
            wait_obj_    = nullptr;
            return Status::Ok;
        }

        /// Полный сброс в состояние «не инициализирована». Нужен для
        /// статических TCB (например idle-задачи планировщика), которые
        /// нельзя пересоздать присваиванием.
        void reset() noexcept {
            sp_          = nullptr;
            fn_          = nullptr;
            name_        = nullptr;
            stack_words_ = 0;
            priority_    = 0;
            type_        = task_type::static_task;
            state_       = task_state::idle;
            ctx_         = nullptr;
            delay_ticks_ = 0;
            core_        = kAnyCore;
            wait_obj_    = nullptr;
        }

        void save_sp(uint32_t* sp) noexcept { sp_ = sp; }
        uint32_t* load_sp() const noexcept { return sp_; }

        // ── Планировщик ──
        /// Перевести задачу в готовность. false, если задача не настроена
        /// или стек не инициализирован: иначе задача попадёт в ready-очередь
        /// с sp_ == nullptr и уронит систему при первой загрузке контекста.
        bool start() noexcept {
            if (fn_ == nullptr || sp_ == nullptr) return false;
            if (state_ != task_state::idle && state_ != task_state::done) return false;
            delay_ticks_ = 0;
            state_       = task_state::ready;
            return true;
        }

        void set_running() noexcept { state_ = task_state::running; }
        void set_done()    noexcept { state_ = task_state::done; }

        void suspend() noexcept {
            if (state_ == task_state::running) state_ = task_state::suspended;
        }
        void resume() noexcept {
            if (state_ == task_state::suspended) state_ = task_state::ready;
        }

        bool sleep_for(uint32_t ticks) noexcept {
            if (state_ != task_state::running || ticks == 0u) return false;
            state_       = task_state::sleeping;
            delay_ticks_ = ticks;
            return true;
        }

        bool wake_from_sleep() noexcept {
            if (state_ != task_state::sleeping) return false;
            state_       = task_state::ready;
            delay_ticks_ = 0;
            return true;
        }

        /// Блокировка на объекте. ticks == 0 — без таймаута (blocked).
        bool block_on(void* obj, uint32_t ticks) noexcept {
            if (state_ != task_state::running) return false;
            wait_obj_    = obj;
            delay_ticks_ = ticks;
            state_       = (ticks == 0u) ? task_state::blocked : task_state::sleeping;
            return true;
        }

        bool unblock() noexcept {
            if (state_ != task_state::blocked && state_ != task_state::sleeping) return false;
            wait_obj_    = nullptr;
            state_       = task_state::ready;
            delay_ticks_ = 0;
            return true;
        }

        // ── Время ──
        uint32_t delay_ticks() const noexcept { return delay_ticks_; }

        /// Декремент задержки; true, когда задержка истекла.
        ///
        /// Декремент ПРЕФИКСНЫЙ: sleep_for(3) обязан разбудить задачу ровно
        /// через 3 тика. Прежний постфиксный вариант возвращал true только
        /// на 4-м тике — задача просыпалась на тик позже запрошенного
        /// (найдено тестом sleep/wake).
        bool tick_delay() noexcept { return advance_delay(1u); }

        /// Декремент задержки сразу на ticks тиков; true, когда задержка
        /// истекла (в том числе «истекла ещё внутри интервала»).
        ///
        /// Нужен там, где монотонное время ОС продвинулось СКАЧКОМ: после
        /// глубокого сна ядро не тикало, и вычитать по одному тику за раз
        /// пришлось бы миллисекунды на каждую проспанную секунду. При
        /// ticks == 1 семантика в точности совпадает с прежним tick_delay().
        bool advance_delay(uint32_t ticks) noexcept {
            if (state_ != task_state::sleeping) return false;
            if (delay_ticks_ > ticks) {
                delay_ticks_ -= ticks;
                return false;
            }
            delay_ticks_ = 0u;
            return true;
        }

        // ── Accessors ──
        task_func   fn()          const noexcept { return fn_; }
        void*       ctx()         const noexcept { return ctx_; }
        const char* name()        const noexcept { return name_; }
        uint32_t    priority()    const noexcept { return priority_; }
        uint32_t    stack_words() const noexcept { return stack_words_; }
        task_type   type()        const noexcept { return type_; }
        task_state  state()       const noexcept { return state_; }
        void*       wait_obj()    const noexcept { return wait_obj_; }

        uint32_t    core()        const noexcept { return core_; }
        bool        is_pinned()   const noexcept { return core_ != kAnyCore; }

        bool is_ready()    const noexcept { return state_ == task_state::ready; }
        bool is_running()  const noexcept { return state_ == task_state::running; }
        bool is_sleeping() const noexcept { return state_ == task_state::sleeping; }
        bool is_blocked()  const noexcept { return state_ == task_state::blocked; }
        bool is_done()     const noexcept { return state_ == task_state::done; }
        bool is_stopped()  const noexcept {
            return state_ == task_state::done || state_ == task_state::idle;
        }
    };

    /// Очередь планировщика: использует унаследованный узел.
    using task_queue_list = intrusive_list<task, /*Embedded*/ true>;

    /// Реестр всех задач.
    using task_registry = object_registry<task, kMaxTasks>;

}
