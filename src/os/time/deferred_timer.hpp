// deferred_timer.hpp
// ---------------------------------------------------------------------------
// Таймер, колбэк которого исполняется НЕ в прерывании.
//
// TimerModule вызывает cb прямо из TickIsr. Если в cb взять мьютекс, подождать
// или сходить в Flash — это ошибка, которую легко не заметить. DeferredTimer
// подставляет в TimerModule короткий ISR-колбэк, который только кладёт работу
// в WorkQueue; пользовательский колбэк выполняется в задаче.
// ---------------------------------------------------------------------------
#pragma once

#include "timer.hpp"
#include "../config/time_config.hpp"
#include "../core/work/work_queue.hpp"
#include "../core/primitives/types.hpp"

namespace embed {

template <Size N>
class DeferredTimer {
    using Timer = config::OsTimer;

    WorkQueue<N>*      wq_ = nullptr;
    Timer::TimerHandle handle_{};
    Callback_h         cb_ = nullptr;
    void*              arg_ = nullptr;
    bool               created_ = false;
    atomic_t<uint32_t> dropped_{0u};

    static void invoke(void* ctx) noexcept {
        auto* self = static_cast<DeferredTimer*>(ctx);
        if (self->cb_ != nullptr) self->cb_(self->arg_);
    }

    static void on_timer(void* ctx) noexcept {
        auto* self = static_cast<DeferredTimer*>(ctx);
        if (self->wq_ == nullptr) return;
        if (!self->wq_->post_from_isr(&invoke, self)) {
            (void)self->dropped_.fetch_add(1u, MemoryOrderRelaxed);
        }
    }

public:
    DeferredTimer() noexcept = default;
    DeferredTimer(const DeferredTimer&)            = delete;
    DeferredTimer& operator=(const DeferredTimer&) = delete;

    /// Создать таймер поверх очереди работ.
    /// WorkQueue и DeferredTimer обязаны жить дольше таймера.
    Status create(WorkQueue<N>& wq, uint32_t period_ms, bool periodic,
                  Callback_h cb, void* arg = nullptr) noexcept {
        if (created_) return Status::AlreadyExists;
        if (cb == nullptr) return Status::InvalidArg;

        wq_ = &wq;
        cb_ = cb;
        arg_ = arg;

        const Status st = Timer::CreateTimer(period_ms, periodic, &on_timer,
                                             this, handle_);
        if (st != Status::Ok) {
            wq_ = nullptr;
            cb_ = nullptr;
            arg_ = nullptr;
            return st;
        }
        created_ = true;
        return Status::Ok;
    }

    Status start() noexcept {
        if (!created_) return Status::Unavailable;
        return Timer::StartTimer(handle_);
    }

    Status stop() noexcept {
        if (!created_) return Status::Unavailable;
        return Timer::StopTimer(handle_);
    }

    Status remove() noexcept {
        if (!created_) return Status::Unavailable;
        const Status st = Timer::RemoveTimer(handle_);
        created_ = false;
        wq_ = nullptr;
        cb_ = nullptr;
        arg_ = nullptr;
        return st;
    }

    bool     created() const noexcept { return created_; }
    uint32_t dropped() const noexcept { return dropped_.load(MemoryOrderRelaxed); }
};

} // namespace embed
