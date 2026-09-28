#pragma once
#include "../../common.hpp"
#include "types.hpp"

namespace embed {

    template <typename T, uint32_t N>
    class RingBuffer {
        static_assert(N >= 2,        "RingBuffer: N must be >= 2");
        static_assert((N & (N-1)) == 0, "RingBuffer: N must be a power of two");
    public:
        static constexpr Size capacity() noexcept { return N - 1; }
        RingBuffer() noexcept = default;
        RingBuffer(const RingBuffer&)            = delete;
        RingBuffer& operator=(const RingBuffer&) = delete;

        bool push(const T& v) noexcept {
            const auto next_head = (head_.load(MemoryOrderRelaxed) + 1) & kMask;
            if (next_head == tail_cached_) {
                tail_cached_ = tail_.load(MemoryOrderAcquire);
                if (next_head == tail_cached_) return false;
            }
            buf_[head_.load(MemoryOrderRelaxed)] = v;
            head_.store(next_head, MemoryOrderRelease);
            return true;
        }

        bool push(T&& v) noexcept {
            const auto next_head = (head_.load(MemoryOrderRelaxed) + 1) & kMask;
            if (next_head == tail_cached_) {
                tail_cached_ = tail_.load(MemoryOrderAcquire);
                if (next_head == tail_cached_) return false;
            }
            buf_[head_.load(MemoryOrderRelaxed)] = static_cast<T&&>(v);
            head_.store(next_head, MemoryOrderRelease);
            return true;
        }

        bool pop(T& out) noexcept {
            const auto t = tail_.load(MemoryOrderRelaxed);
            if (t == head_cached_) {
                head_cached_ = head_.load(MemoryOrderAcquire);
                if (t == head_cached_) return false;
            }
            out = static_cast<T&&>(buf_[t]);
            tail_.store((t + 1) & kMask, MemoryOrderRelease);
            return true;
        }

        T pop() noexcept {
            T v{};
            (void)pop(v);
            return v;
        }

        bool peek(T& out) const noexcept {
            const auto t = tail_.load(MemoryOrderRelaxed);
            if (t == head_.load(MemoryOrderAcquire)) return false;
            out = buf_[t];
            return true;
        }

        bool empty() const noexcept {
            return head_.load(MemoryOrderAcquire) == tail_.load(MemoryOrderAcquire);
        }

        bool full() const noexcept {
            return ((head_.load(MemoryOrderAcquire) + 1) & kMask) == tail_.load(MemoryOrderAcquire);
        }

        Size size() const noexcept {
            return (head_.load(MemoryOrderAcquire) - tail_.load(MemoryOrderAcquire)) & kMask;
        }

        void clear() noexcept {
            head_.store(0, MemoryOrderRelaxed);
            tail_.store(0, MemoryOrderRelaxed);
            head_cached_ = 0;
            tail_cached_ = 0;
        }

    private:
        static constexpr Size kMask = N - 1;
        embed::atomic_t<Size> head_{0};
        embed::atomic_t<Size> tail_{0};
        Size head_cached_{0};
        Size tail_cached_{0};
        T buf_[N]{};
    };

}
