// message_queue.hpp
// ---------------------------------------------------------------------------
// Статическая очередь сообщений с блокирующими push/pop и ISR-вариантами.
//
// ЗАЧЕМ: в ядре был RingBuffer, но это КОНТЕЙНЕР, а не примитив: с ним
// приложению приходилось вручную склеивать «очередь + семафор», и оно
// наступало на потерянное пробуждение. Здесь проверка места и сон — одна
// критическая секция, очередь ожидания — по приоритету, таймаут — абсолютный.
// ---------------------------------------------------------------------------
#pragma once

#include "../primitives/mutex.hpp"      // sched_api, Lock
#include "../task/task.hpp"
#include <new>

namespace embed {

template <typename T, Size N>
class MessageQueue {
    static_assert(N >= 1u, "MessageQueue: ёмкость должна быть >= 1");

    // Сырое хранилище, а не T buf_[N]: MessageQueue не требует
    // default-constructible T. Элемент создаётся placement new при push и
    // разрушается при pop.
    alignas(T) unsigned char buf_[N * sizeof(T)]{};
    Size       head_  = 0u;   // откуда читать
    Size       tail_  = 0u;   // куда писать
    Size       count_ = 0u;
    wait_queue readers_;
    wait_queue writers_;

    T* at(Size i) noexcept {
        return reinterpret_cast<T*>(&buf_[i * sizeof(T)]);
    }
    const T* at(Size i) const noexcept {
        return reinterpret_cast<const T*>(&buf_[i * sizeof(T)]);
    }

    static uint32_t remaining_ticks(uint64_t deadline) noexcept {
        if (deadline == 0u) return 0u;
        const uint64_t now = sched_api::now_ticks();
        if (now >= deadline) return 0u;
        const uint64_t left = deadline - now;
        return (left > 0xFFFFFFFFu) ? 0xFFFFFFFFu : static_cast<uint32_t>(left);
    }

public:
    MessageQueue() noexcept = default;
    ~MessageQueue() noexcept {
        for (Size i = 0u; i < count_; ++i) {
            at((head_ + i) % N)->~T();
        }
    }
    MessageQueue(const MessageQueue&)            = delete;
    MessageQueue& operator=(const MessageQueue&) = delete;

    static constexpr Size capacity() noexcept { return N; }
    Size size()  const noexcept { Lock l; return count_; }
    bool empty() const noexcept { Lock l; return count_ == 0u; }
    bool full()  const noexcept { Lock l; return count_ == N; }

    /// Положить сообщение. ticks == 0 — без таймаута.
    /// Возврат: Ok | Timeout | Unavailable | Fault | AlreadyExists.
    Status push(const T& value, uint32_t ticks = 0) noexcept {
        const uint64_t deadline = (ticks != 0u)
            ? (sched_api::now_ticks() + static_cast<uint64_t>(ticks))
            : 0u;

        for (;;) {
            Lock l;

            if (count_ < N) {
                ::new (static_cast<void*>(at(tail_))) T(value);
                tail_ = (tail_ + 1u) % N;
                ++count_;
                (void)sched_api::unblock_one(readers_);
                return Status::Ok;
            }

            TaskHandler self = sched_api::current();
            if (self == nullptr) return Status::Unavailable;

            if (deadline != 0u) {
                const uint32_t left = remaining_ticks(deadline);
                if (left == 0u) return Status::Timeout;
                const Status st =
                    sched_api::block_on_locked(l, writers_, this, left);
                if (st != Status::Ok) return st;
            } else {
                const Status st =
                    sched_api::block_on_locked(l, writers_, this, 0u);
                if (st != Status::Ok) return st;
            }
        }
    }

    /// Забрать сообщение. ticks == 0 — без таймаута.
    Status pop(T& out, uint32_t ticks = 0) noexcept {
        const uint64_t deadline = (ticks != 0u)
            ? (sched_api::now_ticks() + static_cast<uint64_t>(ticks))
            : 0u;

        for (;;) {
            Lock l;

            if (count_ > 0u) {
                T* item = at(head_);
                out = *item;
                item->~T();
                head_ = (head_ + 1u) % N;
                --count_;
                (void)sched_api::unblock_one(writers_);
                return Status::Ok;
            }

            TaskHandler self = sched_api::current();
            if (self == nullptr) return Status::Unavailable;

            if (deadline != 0u) {
                const uint32_t left = remaining_ticks(deadline);
                if (left == 0u) return Status::Timeout;
                const Status st =
                    sched_api::block_on_locked(l, readers_, this, left);
                if (st != Status::Ok) return st;
            } else {
                const Status st =
                    sched_api::block_on_locked(l, readers_, this, 0u);
                if (st != Status::Ok) return st;
            }
        }
    }

    bool try_push(const T& value) noexcept {
        Lock l;
        if (count_ >= N) return false;
        ::new (static_cast<void*>(at(tail_))) T(value);
        tail_ = (tail_ + 1u) % N;
        ++count_;
        (void)sched_api::unblock_one(readers_);
        return true;
    }

    bool try_pop(T& out) noexcept {
        Lock l;
        if (count_ == 0u) return false;
        T* item = at(head_);
        out = *item;
        item->~T();
        head_ = (head_ + 1u) % N;
        --count_;
        (void)sched_api::unblock_one(writers_);
        return true;
    }

    /// Из прерывания: не блокируется.
    bool push_from_isr(const T& value) noexcept {
        Lock l;
        if (count_ >= N) return false;
        ::new (static_cast<void*>(at(tail_))) T(value);
        tail_ = (tail_ + 1u) % N;
        ++count_;
        (void)sched_api::unblock_one(readers_);
        return true;
    }

    bool pop_from_isr(T& out) noexcept {
        Lock l;
        if (count_ == 0u) return false;
        T* item = at(head_);
        out = *item;
        item->~T();
        head_ = (head_ + 1u) % N;
        --count_;
        (void)sched_api::unblock_one(writers_);
        return true;
    }

    Size readers() const noexcept { Lock l; return readers_.size(); }
    Size writers() const noexcept { Lock l; return writers_.size(); }
};

} // namespace embed
