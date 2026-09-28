// scheduler.hpp
#pragma once

#include "task.hpp"

namespace embed {

    // Platform-dependent — реализация в platform.cpp
    uint32_t platform_core_count() noexcept;
    uint32_t platform_current_core() noexcept;
    void platform_start_first_task(uint32_t* sp) noexcept;
    void platform_trigger_context_switch() noexcept;
    void platform_critical_enter() noexcept;
    void platform_critical_exit() noexcept;
    uint32_t* platform_save_context() noexcept;
    void platform_load_context(uint32_t* sp) noexcept;

    // Планировщик
    static constexpr uint32_t kMaxCores    = 4;
    static constexpr uint32_t kMaxPriority = 16;
    class scheduler {
    private:
        struct core_state {
            TaskHandler          current;
            intrusive_list<task> ready[kMaxPriority];
            uint32_t             ready_count;
            uint32_t             core_id;
        };

        core_state          cores_[kMaxCores]{};
        uint32_t            core_count_ = 0;

        intrusive_list<task> sleep_list_;
        intrusive_list<task> all_tasks_;

        bool                started_ = false;

        uint32_t find_least_loaded_core() noexcept;
        TaskHandler pick_next(core_state& core) noexcept;
        void ready_push(core_state& core, TaskHandler t) noexcept;
        void ready_remove(core_state& core, TaskHandler t) noexcept;
        void check_preempt(core_state& core) noexcept;
        void process_sleep_list() noexcept;

    public:
        scheduler() noexcept = default;
        scheduler(const scheduler&)            = delete;
        scheduler& operator=(const scheduler&) = delete;

        // Инит 
        void init() noexcept;

        // Работа с задачами
        bool add_task(TaskHandler t, uint32_t core_id = 0xFFFFFFFF) noexcept;
        void remove_task(TaskHandler t) noexcept;

        // Lifecycle
        void start() noexcept;
        void yield() noexcept;

        // Task API
        void sleep() noexcept;
        void sleep_for(uint32_t ticks) noexcept;
        void wakeup(TaskHandler t, uint32_t core_id = 0xFFFFFFFF) noexcept;
        void exit() noexcept;

        // Тик (SysTick_Handler)
        void tick() noexcept;

        // Смена контекста (PendSV_Handler)
        void context_switch() noexcept;

        // Очередь задач
        TaskHandler current_task() noexcept {
            return cores_[platform_current_core()].current;
        }
        Size task_count()  const noexcept { return all_tasks_.size(); }
        uint32_t  core_count()  const noexcept { return core_count_; }
        uint32_t  ready_count(uint32_t core_id) const noexcept {
            if (core_id >= core_count_) return 0;
            return cores_[core_id].ready_count;
        }
        Size sleep_count() const noexcept { return sleep_list_.size(); }
    };

}
