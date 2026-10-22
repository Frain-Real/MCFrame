// mailbox.hpp
// ---------------------------------------------------------------------------
// Почтовый ящик: одно место, «последнее значение важнее предыдущего».
//
// Отличие от MessageQueue<T,1>: post() НЕ блокируется и перезаписывает
// значение. Это типичный «последний приказ»/«последний сэмпл»: потребитель
// обязан увидеть актуальное, а не очередь устаревших.
// ---------------------------------------------------------------------------
#pragma once

#include "../primitives/mutex.hpp"
#include "../task/task.hpp"

namespace embed {

template <typename T>
class Mailbox {
    T          value_{};
    bool       has_ = false;
    wait_queue waiters_;

    static uint32_t remaining_ticks(uint64_t deadline) noexcept {
        if (deadline == 0u) return 0u;
        const uint64_t now = sched_api::now_ticks();
        if (now >= deadline) return 0u;
        const uint64_t left = deadline - now;
        return (left > 0xFFFFFFFFu) ? 0xFFFFFFFFu : static_cast<uint32_t>(left);
    }

public:
    Mailbox() noexcept = default;
    Mailbox(const Mailbox&)            = delete;
    Mailbox& operator=(const Mailbox&) = delete;

    /// Положить значение (перезаписать). Никогда не блокируется.
    void post(const T& value) noexcept {
        Lock l;
        value_ = value;
        has_ = true;
        (void)sched_api::unblock_one(waiters_);
    }

    void post_from_isr(const T& value) noexcept {
        Lock l;
        value_ = value;
        has_ = true;
        (void)sched_api::unblock_one(waiters_);
    }

    /// Забрать значение, ожидая появления. ticks == 0 — без таймаута.
    Status pend(T& out, uint32_t ticks = 0) noexcept {
        const uint64_t deadline = (ticks != 0u)
            ? (sched_api::now_ticks() + static_cast<uint64_t>(ticks))
            : 0u;

        for (;;) {
            Lock l;
            if (has_) {
                out = value_;
                has_ = false;
                return Status::Ok;
            }

            TaskHandler self = sched_api::current();
            if (self == nullptr) return Status::Unavailable;

            if (deadline != 0u) {
                const uint32_t left = remaining_ticks(deadline);
                if (left == 0u) return Status::Timeout;
                const Status st =
                    sched_api::block_on_locked(l, waiters_, this, left);
                if (st != Status::Ok) return st;
            } else {
                const Status st =
                    sched_api::block_on_locked(l, waiters_, this, 0u);
                if (st != Status::Ok) return st;
            }
        }
    }

    bool try_pend(T& out) noexcept {
        Lock l;
        if (!has_) return false;
        out = value_;
        has_ = false;
        return true;
    }

    bool peek(T& out) const noexcept {
        Lock l;
        if (!has_) return false;
        out = value_;
        return true;
    }

    void clear() noexcept {
        Lock l;
        has_ = false;
    }

    bool has_value() const noexcept { Lock l; return has_; }
    Size waiters() const noexcept { Lock l; return waiters_.size(); }
};

} // namespace embed
