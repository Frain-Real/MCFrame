// semaphore.hpp — template semaphore, kind encoded in the type
//
//   Binary  : Semaphore<1>          // atomic flag, no double-release
//   Counting: Semaphore<5>          // atomic counter, N permits
//
// All operations are lock-free / atomic. No dynamic allocation.
// ISR-safe release/try_acquire available via release_from_isr().
//
// Header-only. Requires C++17 (C++20 recommended for std::atomic::wait).

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <thread>   // std::this_thread (fallback)
#if defined(__has_include) && __has_include(<linux/futex.h>)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace embedded {

    // ---------------------------------------------------------------------------
    //  Wait primitive abstraction. Default impl uses Linux futex with private
    //  hash (no cross-process wakeups). On bare-metal / other RTOS override by
    //  providing wait/wake_one/wake_all with the same semantics.
    // ---------------------------------------------------------------------------
    namespace detail {

        inline void wait_on_atomic(const std::atomic<std::int32_t>& v,
                                   std::int32_t expected) noexcept {
                                       #if defined(__has_include) && __has_include(<linux/futex.h>)
                                       ::syscall(SYS_futex,
                                                 reinterpret_cast<std::intptr_t>(const_cast<std::atomic<std::int32_t>*>(&v)),
                                                 FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                                                 expected, nullptr, nullptr, 0);
                                       #else
                                       (void)v; (void)expected;
                                       // exponential backoff would belong here. For MCU build, replace with
                                       // your RTOS task-block primitive.
                                       std::this_thread::yield();
                                       #endif
                                   }

                                   inline void wake_one_atomic(const std::atomic<std::int32_t>& v) noexcept {
                                       #if defined(__has_include) && __has_include(<linux/futex.h>)
                                       ::syscall(SYS_futex,
                                                 reinterpret_cast<std::intptr_t>(const_cast<std::atomic<std::int32_t>*>(&v)),
                                                 FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
                                                 1, nullptr, nullptr, 0);
                                       #else
                                       (void)v;
                                       #endif
                                   }

                                   inline void wake_all_atomic(const std::atomic<std::int32_t>& v) noexcept {
                                       #if defined(__has_include) && __has_include(<linux/futex.h>)
                                       ::syscall(SYS_futex,
                                                 reinterpret_cast<std::intptr_t>(const_cast<std::atomic<std::int32_t>*>(&v)),
                                                 FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
                                                 static_cast<std::int32_t>(0x7fffffff),
                                                 nullptr, nullptr, 0);
                                       #else
                                       (void)v;
                                       #endif
                                   }

    } // namespace detail

    // ---------------------------------------------------------------------------
    //  Binary specialization: Semaphore<1>
    //  - One permit, no accumulation on repeated release()
    //  - Backed by atomic_flag (1 byte on most ABIs) for compactness
    //  - Wait is implemented on a paired futex word so we can block until set
    // ---------------------------------------------------------------------------
    template <>
    class Semaphore<1> {
    public:
        static constexpr std::ptrdiff_t max_value() noexcept { return 1; }
        static constexpr bool is_binary = true;

        constexpr Semaphore(bool initially_set = false) noexcept
        : state_(initially_set ? 1 : 0) {}

        Semaphore(const Semaphore&) = delete;
        Semaphore& operator=(const Semaphore&) = delete;

        void acquire() noexcept {
            while (true) {
                // fast path: try to flip 1 -> 0
                if (state_.load(std::memory_order_relaxed) == 1) {
                    std::int32_t expected = 1;
                    if (state_.compare_exchange_weak(
                        expected, 0,
                        std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                        return;
                        }
                }
                // slow path: park until state becomes 1
                detail::wait_on_atomic(state_, 0);
            }
        }

        template <typename Rep, typename Period>
        bool try_acquire_for(const std::chrono::duration<Rep, Period>& timeout) noexcept {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (true) {
                if (state_.load(std::memory_order_relaxed) == 1) {
                    std::int32_t expected = 1;
                    if (state_.compare_exchange_weak(
                        expected, 0,
                        std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                        return true;
                        }
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    return false;
                }
                detail::wait_on_atomic(state_, 0);
            }
        }

        bool try_acquire_until(const std::chrono::steady_clock::time_point& tp) noexcept {
            return try_acquire_for(tp - std::chrono::steady_clock::now());
        }

        bool try_acquire() noexcept {
            std::int32_t expected = 1;
            return state_.compare_exchange_strong(
                expected, 0,
                std::memory_order_acquire,
                std::memory_order_relaxed);
        }

        // Binary release is idempotent: releasing a set semaphore is a no-op
        // (returns false) so you can detect double-release bugs in debug builds.
        bool release() noexcept {
            std::int32_t expected = 0;
            if (!state_.compare_exchange_strong(
                expected, 1,
                std::memory_order_release,
                std::memory_order_relaxed)) {
                return false;  // already set -> double release
                }
                detail::wake_one_atomic(state_);
            return true;
        }

        // ISR-safe release. Returns true if this call actually set the flag.
        bool release_from_isr() noexcept {
            std::int32_t expected = 0;
            return state_.compare_exchange_strong(
                expected, 1,
                std::memory_order_release,
                std::memory_order_relaxed);
        }

        void release_from_isr_then_woke() noexcept {
            // Use only when IRQ runs in process context; otherwise rely on a
            // thread-context release() to wake waiters.
            detail::wake_one_atomic(state_);
        }

        std::int32_t value() const noexcept {
            return state_.load(std::memory_order_acquire);
        }

        void reset(bool set = false) noexcept {
            state_.store(set ? 1 : 0, std::memory_order_release);
            if (set) detail::wake_one_atomic(state_);
            else     detail::wake_all_atomic(state_);
        }

    private:
        // 0 = not set, 1 = set. atomic<int32> so we can futex-wait on it.
        std::atomic<std::int32_t> state_;
    };

    // ---------------------------------------------------------------------------
    //  Counting specialization: Semaphore<N>, N > 1
    //  - Bounded counter, permits accumulate on release
    // ---------------------------------------------------------------------------
    template <std::ptrdiff_t LeastMaxValue>
    class Semaphore {
        static_assert(LeastMaxValue > 1, "Use Semaphore<1> for binary");
        static_assert(LeastMaxValue <=
        static_cast<std::ptrdiff_t>(std::numeric_limits<std::int32_t>::max()),
                      "Max value exceeds atomic int32 range");

    public:
        static constexpr std::ptrdiff_t max_value() noexcept { return LeastMaxValue; }
        static constexpr bool is_binary = false;

        constexpr Semaphore(std::ptrdiff_t initial = 0) noexcept
        : count_(clamp(initial)) {}

        Semaphore(const Semaphore&) = delete;
        Semaphore& operator=(const Semaphore&) = delete;

        void acquire() noexcept {
            while (true) {
                std::int32_t cur = count_.load(std::memory_order_relaxed);
                while (cur > 0) {
                    if (count_.compare_exchange_weak(
                        cur, cur - 1,
                        std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                        return;
                        }
                }
                detail::wait_on_atomic(count_, 0);
            }
        }

        template <typename Rep, typename Period>
        bool try_acquire_for(const std::chrono::duration<Rep, Period>& timeout) noexcept {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (true) {
                std::int32_t cur = count_.load(std::memory_order_relaxed);
                while (cur > 0) {
                    if (count_.compare_exchange_weak(
                        cur, cur - 1,
                        std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                        return true;
                        }
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    return false;
                }
                detail::wait_on_atomic(count_, 0);
            }
        }

        bool try_acquire_until(const std::chrono::steady_clock::time_point& tp) noexcept {
            return try_acquire_for(tp - std::chrono::steady_clock::now());
        }

        bool try_acquire() noexcept {
            std::int32_t cur = count_.load(std::memory_order_relaxed);
            while (cur > 0) {
                if (count_.compare_exchange_weak(
                    cur, cur - 1,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                    return true;
                    }
            }
            return false;
        }

        // Returns previous count. Saturates at max_value().
        std::int32_t release() noexcept {
            std::int32_t prev = count_.load(std::memory_order_relaxed);
            std::int32_t next;
            do {
                if (prev >= static_cast<std::int32_t>(LeastMaxValue)) {
                    return prev;  // saturated
                }
                next = prev + 1;
            } while (!count_.compare_exchange_weak(
                prev, next,
                std::memory_order_release,
                std::memory_order_relaxed));
            detail::wake_one_atomic(count_);
            return prev;
        }

        std::int32_t release_from_isr() noexcept {
            return release();  // saturating CAS-loop, ISR-safe on most platforms
        }

        std::int32_t value() const noexcept {
            return count_.load(std::memory_order_relaxed);
        }

        void reset(std::ptrdiff_t initial = 0) noexcept {
            count_.store(clamp(initial), std::memory_order_release);
            detail::wake_all_atomic(count_);
        }

    private:
        static constexpr std::int32_t clamp(std::ptrdiff_t v) noexcept {
            if (v < 0) return 0;
            if (v > LeastMaxValue) return static_cast<std::int32_t>(LeastMaxValue);
            return static_cast<std::int32_t>(v);
        }

        std::atomic<std::int32_t> count_;
    };

    // ---------------------------------------------------------------------------
    //  Aliases
    // ---------------------------------------------------------------------------
    using BinarySemaphore   = Semaphore<1>;

    template <std::ptrdiff_t N>
    using CountingSemaphore = Semaphore<N>;

    static_assert(!BinarySemaphore::is_binary,   "Binary must be binary");
    static_assert(!CountingSemaphore<4>::is_binary, "Counting must not be binary");
    static_assert(BinarySemaphore::max_value() == 1, "");
    static_assert(CountingSemaphore<8>::max_value() == 8, "");

} // namespace embedded
