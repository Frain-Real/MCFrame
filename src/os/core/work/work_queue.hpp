// work_queue.hpp
// ---------------------------------------------------------------------------
// Очередь работ: ISR/таймер кладёт функцию, задача — исполняет.
//
// ЗАЧЕМ: колбэки TimerModule::TickIsr и обработчики прерываний выполняются
// в контексте исключения, где нельзя ни ждать, ни брать мьютекс, ни делать
// долгую работу. WorkQueue переносит работу в задачу: post() короткий, а
// исполнение — обычный контекст с полным API ядра.
//
// Ёмкость статическая, память не выделяется. Стек задачи передаёт
// приложение — как и для любой другой задачи.
// ---------------------------------------------------------------------------
#pragma once

#include "../primitives/mutex.hpp"
#include "../task/task.hpp"
#include "../task/sheduler.hpp"

namespace embed {

template <Size N>
class WorkQueue {
    static_assert(N >= 1u, "WorkQueue: ёмкость должна быть >= 1");

    using Fn = void (*)(void*);

    struct Item {
        Fn    fn = nullptr;
        void* ctx = nullptr;
    };

    Item       items_[N]{};
    Size       head_  = 0u;
    Size       tail_  = 0u;
    Size       count_ = 0u;

    task        worker_{};
    bool        started_ = false;

    bool pop_locked(Item& out) noexcept {
        if (count_ == 0u) return false;
        out = items_[head_];
        head_ = (head_ + 1u) % N;
        --count_;
        return true;
    }

    static void trampoline(void* ctx) noexcept {
        auto* self = static_cast<WorkQueue*>(ctx);
        for (;;) {
            Item item;
            bool has = false;
            {
                Lock l;
                has = self->pop_locked(item);
            }
            if (has) {
                if (item.fn != nullptr) item.fn(item.ctx);
                continue;
            }
            // post() разбудит; если работа пришла до входа в sleep(),
            // wake_pending не даст потерять её и sleep вернётся сразу.
            (void)scheduler::instance().sleep();
        }
    }

public:
    WorkQueue() noexcept = default;
    WorkQueue(const WorkQueue&)            = delete;
    WorkQueue& operator=(const WorkQueue&) = delete;

    /// Создать и зарегистрировать задачу-исполнитель.
    /// Стек должен жить дольше очереди (статический массив приложения).
    Status start(const char* name, uint32_t* stack, Size stack_words,
                 uint32_t priority = 5u) noexcept {
        if (started_) return Status::AlreadyExists;
        if (stack == nullptr || stack_words == 0u) return Status::InvalidArg;

        Status st = worker_.init(&trampoline, this, name, stack_words, priority);
        if (st != Status::Ok) return st;

        if (!scheduler::instance().add_task(&worker_, stack, stack_words)) {
            return Status::NoMemory;
        }
        started_ = true;
        return Status::Ok;
    }

    /// Поставить работу. false — очередь заполнена.
    bool post(Fn fn, void* ctx = nullptr) noexcept {
        if (fn == nullptr) return false;

        bool wake = false;
        {
            Lock l;
            if (count_ >= N) return false;
            items_[tail_] = Item{fn, ctx};
            tail_ = (tail_ + 1u) % N;
            ++count_;
            wake = (count_ == 1u);
        }
        if (wake && started_) {
            (void)scheduler::instance().wakeup(&worker_);
        }
        return true;
    }

    bool post_from_isr(Fn fn, void* ctx = nullptr) noexcept {
        if (fn == nullptr) return false;

        bool wake = false;
        {
            Lock l;      // в прерывании Lock сохраняет/восстанавливает PRIMASK
            if (count_ >= N) return false;
            items_[tail_] = Item{fn, ctx};
            tail_ = (tail_ + 1u) % N;
            ++count_;
            wake = (count_ == 1u);
        }
        if (wake && started_) {
            (void)scheduler::instance().wakeup(&worker_);
        }
        return true;
    }

    /// Исполнить одну работу в ТЕКУЩЕМ контексте (для своего цикла/тестов).
    bool run_one() noexcept {
        Item item;
        {
            Lock l;
            if (!pop_locked(item)) return false;
        }
        if (item.fn != nullptr) item.fn(item.ctx);
        return true;
    }

    /// Исполнить всё, что есть (без сна).
    Size drain() noexcept {
        Size n = 0u;
        while (run_one()) ++n;
        return n;
    }

    Size size()  const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0u; }
    bool full()  const noexcept { return count_ == N; }
    bool started() const noexcept { return started_; }
    TaskHandler worker_task() noexcept { return &worker_; }
};

} // namespace embed
