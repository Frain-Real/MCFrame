// task.hpp
#pragma once

#include "intrusive_list.hpp"
#include <cstdint>

namespace embed {

    /// Тип задачи.
    enum class task_type : uint8_t {
        static_task,   // TCB и стек выделены статически (глобально)
        dynamic_task   // TCB и стек выделены динамически (куча)
    };

    /// Состояние задачи с вытеснением.
    enum class task_state : uint8_t {
        idle,       // неактивна
        ready,      // ждёт запуска
        running,    // выполняется сейчас
        suspended,  // вытеснена / ожидает события
        sleeping,   // спит N тиков (delay)
        done        // завершена
    };

    // Предварительное объявление
    class task;

    /// Handle на задачу — используется планировщиком и пользователем.
    using TaskHandler = task*;

    /// Задача. Только метаданные.
    /// Наследуется от list_node<task> для встраивания в intrusive_list.
    /// Стеком и контекстом управляет менеджер памяти.
    class task : public list_node<task> {
    public:
        using fn_t = void (*)(void* ctx);

    private:
        uint32_t*   sp_          = nullptr;   // сохранённый SP
        fn_t        fn_          = nullptr;   // функция задачи
        const char* name_        = nullptr;   // имя (для отладки)
        uint32_t    priority_    = 0;         // приоритет (0 = высший)
        uint32_t    stack_words_ = 0;         // выделено памяти (слов)
        TaskHandler handle_      = nullptr;   // указатель на себя

        task_type   type_        = task_type::static_task;
        task_state  state_       = task_state::idle;
        void*       ctx_         = nullptr;   // аргумент функции

        uint32_t    delay_ticks_ = 0;         // осталось тиков до пробуждения

    public:
        task() noexcept = default;
        task(const task&)            = delete;
        task& operator=(const task&) = delete;

        // ──────────── Setup ────────────

        void init(fn_t fn, void* ctx,
                  const char* name,
                  uint32_t stack_words,
                  uint32_t priority = 0,
                  task_type type = task_type::static_task) noexcept {
            fn_          = fn;
            ctx_         = ctx;
            name_        = name;
            stack_words_ = stack_words;
            priority_    = priority;
            type_        = type;
            state_       = task_state::idle;
            handle_      = this;
            delay_ticks_ = 0;
        }

        void save_sp(uint32_t* sp) noexcept { sp_ = sp; }
        uint32_t* load_sp() const noexcept { return sp_; }
        
        // ──────────── Lifecycle (планировщик) ────────────

        void start() noexcept {
            if (fn_ && state_ == task_state::idle) state_ = task_state::ready;
        }

        void suspend() noexcept {
            if (state_ == task_state::running) state_ = task_state::suspended;
        }

        void resume() noexcept {
            if (state_ == task_state::suspended) state_ = task_state::ready;
        }

        void sleep_for(uint32_t ticks) noexcept {
            if (state_ == task_state::running && ticks > 0) {
                state_       = task_state::sleeping;
                delay_ticks_ = ticks;
            }
        }

        void wake_from_sleep() noexcept {
            if (state_ == task_state::sleeping) {
                state_       = task_state::ready;
                delay_ticks_ = 0;
            }
        }

        void set_running() noexcept { state_ = task_state::running; }
        void set_done()    noexcept { state_ = task_state::done; }

        void reset() noexcept {
            state_       = task_state::idle;
            delay_ticks_ = 0;
        }

        // ──────────── Delay ────────────

        uint32_t delay_ticks() const noexcept { return delay_ticks_; }

        bool tick_delay() noexcept {
            if (state_ != task_state::sleeping) return false;
            if (delay_ticks_ == 0) return true;
            --delay_ticks_;
            return delay_ticks_ == 0;
        }

        // ──────────── Accessors ────────────

        fn_t        fn()          const noexcept { return fn_; }
void*       ctx()         const noexcept { return ctx_; }
        const char* name()        const noexcept { return name_; }
        uint32_t    priority()    const noexcept { return priority_; }
        uint32_t    stack_words() const noexcept { return stack_words_; }
        TaskHandler handle()      const noexcept { return handle_; }
        task_type   type()        const noexcept { return type_; }
        task_state  state()       const noexcept { return state_; }

        bool is_ready()    const noexcept { return state_ == task_state::ready; }
        bool is_sleeping() const noexcept { return state_ == task_state::sleeping; }
        bool is_done()     const noexcept { return state_ == task_state::done; }
    };

} // namespace embed
