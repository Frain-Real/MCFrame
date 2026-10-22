// event_flags.hpp
// ---------------------------------------------------------------------------
// Группа событий: 32 бита, ожидание «любое» или «все», автоочистка.
//
// ЗАЧЕМ: семафор сигналит «событие», но не говорит КАКОЕ. Приложению часто
// нужно «проснуться, когда UART принял ИЛИ таймер истёк, но не от каждого
// по отдельности». EventFlags — это набор битов + ожидание маски.
// ---------------------------------------------------------------------------
#pragma once

#include "../primitives/mutex.hpp"
#include "../task/task.hpp"

namespace embed {

class EventFlags {
    uint32_t   flags_ = 0u;
    wait_queue waiters_;

    static uint32_t remaining_ticks(uint64_t deadline) noexcept {
        if (deadline == 0u) return 0u;
        const uint64_t now = sched_api::now_ticks();
        if (now >= deadline) return 0u;
        const uint64_t left = deadline - now;
        return (left > 0xFFFFFFFFu) ? 0xFFFFFFFFu : static_cast<uint32_t>(left);
    }

public:
    EventFlags() noexcept = default;
    EventFlags(const EventFlags&)            = delete;
    EventFlags& operator=(const EventFlags&) = delete;

    /// Ждать маску. wait_all == true — нужны ВСЕ биты, иначе любой.
    /// clear_on_exit — снять ожидаемые биты после успешного ожидания.
    /// ticks == 0 — без таймаута.
    Status wait(uint32_t mask, bool wait_all = false,
                bool clear_on_exit = true, uint32_t ticks = 0) noexcept {
        if (mask == 0u) return Status::InvalidArg;

        const uint64_t deadline = (ticks != 0u)
            ? (sched_api::now_ticks() + static_cast<uint64_t>(ticks))
            : 0u;

        for (;;) {
            Lock l;

            const bool ready = wait_all
                ? ((flags_ & mask) == mask)
                : ((flags_ & mask) != 0u);

            if (ready) {
                if (clear_on_exit) flags_ &= ~mask;
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

    /// Выставить биты и разбудить ВСЕХ ожидающих: каждый перепроверит маску.
    void set(uint32_t bits) noexcept {
        Lock l;
        if (bits == 0u) return;
        flags_ |= bits;
        if (!waiters_.empty()) (void)sched_api::unblock_all(waiters_);
    }

    void set_from_isr(uint32_t bits) noexcept {
        Lock l;
        if (bits == 0u) return;
        flags_ |= bits;
        if (!waiters_.empty()) (void)sched_api::unblock_all(waiters_);
    }

    void clear(uint32_t bits) noexcept {
        Lock l;
        flags_ &= ~bits;
    }

    /// Текущие биты. 32-битное чтение атомарно на Cortex-M; для согласованного
    /// снимка с ожиданием используйте wait()/clear().
    uint32_t value() const noexcept { Lock l; return flags_; }
    bool any(uint32_t mask) const noexcept { Lock l; return (flags_ & mask) != 0u; }
    bool all(uint32_t mask) const noexcept { Lock l; return (flags_ & mask) == mask; }
    Size waiters() const noexcept { Lock l; return waiters_.size(); }
};

} // namespace embed
